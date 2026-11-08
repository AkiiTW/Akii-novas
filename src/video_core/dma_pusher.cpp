// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <span>
#include <vector>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/memory.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_sync_profile.h"
#include "video_core/dma_pusher.h"
#include "video_core/command_fetch_order.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/guest_memory.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_cmdpage_diag.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra {

constexpr u32 MacroRegistersStart = 0xE00;
constexpr u32 ComputeInline = 0x6D;

bool DmaPusher::IsMemoryWriter(const Engines::EngineInterface& engine, Engines::EngineTypes type,
                              u32 method) {
    if (type == Engines::EngineTypes::KeplerCompute) {
        return static_cast<const Engines::KeplerCompute&>(engine).MayWriteMemory(method);
    }
    if (type == Engines::EngineTypes::MaxwellDMA) {
        return method >= MacroRegistersStart || engine.execution_mask[method];
    }
    return false;
}

DmaPusher::DmaPusher(Core::System& system_, MemoryManager& memory_manager_, Control::ChannelState& channel_state_)
    : system{system_}
    , memory_manager{memory_manager_}
    , channel_state{channel_state_}
    , signal_sync{false}
    , synced{false}
{}

DmaPusher::~DmaPusher() = default;

void DmaPusher::DispatchCalls() {
    // The sub-index is kept: a channel suspended on a semaphore resumes inside its command list.
    dma_state.is_last_call = true;
    while (system.IsPoweredOn()) {
        if (!Step()) {
            break;
        }
    }
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer([gpu = &system.GPU()] {
            gpu->FlushCommands();
            gpu->OnCommandListEnd();
        }, VideoCommon::GpuSplit::Effect::None);
        split.Publish();
        return;
    }
    system.GPU().FlushCommands();
    system.GPU().OnCommandListEnd();
}

bool DmaPusher::Step() {
    if (!ib_enable || dma_pushbuffer.empty()) {
        return false;
    }
    const u64 suspensions_before = suspensions.load(std::memory_order_relaxed);
    if (!puller.ResumeAcquire(*this)) {
        return false;
    }
    // Lists queued while the channel waited were read from memory, not as submitted: the game
    // writes the commands a list runs before it releases the acquire, so the words at submit time
    // could be older. Reading them as late as the parse is too late in Hyrule Warriors: Age of
    // Imprisonment fights: a command ring the game had already refilled for a later frame, read
    // as this frame's commands (methods on unbound subchannels, garbage QMDs, then a channel
    // waiting forever on a release it never got). The acquire is released now, so the commands
    // are written and the memory not yet reused: copy them here. EDEN_NX2_RESNAPSHOT=0: off.
    if (ResnapshotEnabled() && suspensions.load(std::memory_order_relaxed) != suspensions_before) {
        const u64 now = suspensions.load(std::memory_order_acquire);
        for (CommandList& list : dma_pushbuffer) {
            if (!list.submitted_offsets.empty()) {
                RefreshSnapshot(list, now);
            }
        }
    }

    CommandList& command_list = dma_pushbuffer.front();

    const size_t prefetch_size = command_list.prefetch_command_list.size();
    const size_t command_list_size = command_list.command_lists.size();

    if (prefetch_size == 0 && command_list_size == 0) {
        dma_pushbuffer.pop_front();
        dma_pushbuffer_subindex = 0;
        return true;
    }

    if (prefetch_size > 0) {
        const std::span<const CommandHeader> commands{command_list.prefetch_command_list};
        command_resume_offset +=
            ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        if (puller.HasPendingAcquire()) {
            return false;
        }
        command_resume_offset = 0;
        dma_pushbuffer.pop_front();
        return true;
    }

    auto& current_command = command_list.command_lists[dma_pushbuffer_subindex];
    const CommandListHeader& header = current_command;
    dma_state.dma_get = header.addr;

    if (signal_sync && !synced) {
        std::unique_lock lk(sync_mutex);
        sync_cv.wait(lk, [this]() { return synced; });
        signal_sync = false;
        synced = false;
    }

    if (header.size > 0) {
        if (header.sync && g_nx2_ampere.load(std::memory_order_relaxed)) {
            VideoCommon::GpuSplit::Get().NoteSync();
        }
        // EDEN_NX2_FETCH_DRAIN=1: wait for the back before every entry, as before the split
        // overlapped the two halves again.
        static const bool fetch_drain = std::getenv("EDEN_NX2_FETCH_DRAIN") != nullptr;
        if (fetch_drain && g_nx2_ampere.load(std::memory_order_relaxed)) {
            VideoCommon::GpuSplit::Get().Drain(VideoCommon::DrainSite::FetchDrain);
        }
        bool gpu_written = IsGpuWrittenCommands(dma_state.dma_get,
                                                 size_t{header.size} * sizeof(u32));
        if (subchannels[dma_state.subchannel] && dma_state.method_count) {
            const auto engine = subchannel_type[dma_state.subchannel];
            const bool kepler_payload = engine == Engines::EngineTypes::KeplerCompute && dma_state.method == ComputeInline && dma_state.non_incrementing;
            const bool macro_payload = engine == Engines::EngineTypes::Maxwell3D && dma_state.method >= MacroRegistersStart;
            if (kepler_payload || macro_payload) {
                const size_t words = std::min<size_t>(dma_state.method_count, header.size);
                auto* const engine_state = subchannels[dma_state.subchannel];
                if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
                    // The caches behind IsMemoryDirty belong to the split's back thread.
                    split.Defer([engine_state, memory = &memory_manager, address = GPUVAddr{dma_state.dma_get},
                                 bytes = words * sizeof(u32)] {
                        engine_state->current_dirty = memory->IsMemoryDirty(address, bytes);
                    }, VideoCommon::GpuSplit::Effect::None);
                } else {
                    engine_state->current_dirty = memory_manager.IsMemoryDirty(dma_state.dma_get, words * sizeof(u32));
                }
            }
        }
        if (command_resume_offset == 0) {
            // A method's data continues across GP entries, whatever their level: title 4 ends a
            // main entry after a macro's first word and supplies the remaining parameters in a
            // 5-word subroutine entry. EDEN_NX2_PARK_SPLIT_METHODS restores the old guess (an
            // entry elsewhere is a call that starts clean; resume where the split one ended).
            static const bool park_split_methods = std::getenv("EDEN_NX2_PARK_SPLIT_METHODS") != nullptr;
            if (g_nx2_ampere) {
                if (static u32 split_logs{}; dma_state.method_count != 0 && split_logs < 64) {
                    ++split_logs;
                    LOG_DEBUG(HW_GPU, "Split method {:#x} ({} left) on ch {}: next entry {:#x} size {} raw {:#x} (level bit {}, priv bit {}), expected {:#x}, parked {}",
                             dma_state.method, dma_state.method_count, channel_state.bind_id,
                             u64{header.addr}, u64{header.size}, header.raw, u64{header.is_push_buffer},
                             u64{header.allow_flush}, split_method_end, parked_methods.size());
                    std::array<u32, 8> next_words{};
                    std::array<u32, 4> prev_words{};
                    memory_manager.ReadBlockUnsafe(header.addr, next_words.data(),
                                             std::min<size_t>(header.size, 8) * sizeof(u32));
                    memory_manager.ReadBlockUnsafe(split_method_end - sizeof(prev_words), prev_words.data(),
                                             sizeof(prev_words));
                    LOG_DEBUG(HW_GPU, "  previous entry ends {:08x}; next entry starts {:08x}",
                              fmt::join(prev_words, " "), fmt::join(next_words, " "));
                }
                if (park_split_methods) {
                    ResolveSplitMethod(header.addr);
                }
            }
            if (dma_pushbuffer_subindex < command_list.submit_hashes.size()) {
                CheckSubmitHash(header, command_list.submit_hashes[dma_pushbuffer_subindex]);
            }
        }
        // A resumed entry is read again: the words after the acquire may be new by now.
        // A safe read flushes the caches, which belong to the split's back thread while it runs
        // (the split only starts with unsafe reads; the settings can change mid-game).
        const bool use_safe = !VideoCommon::GpuSplit::Get().Active() &&
                              (Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh() : Settings::IsDMALevelSafe());
        auto& split = VideoCommon::GpuSplit::Get();
        bool list_pages_candidate = false;
        bool list_target_verified_live = false;
        if (!gpu_written && split.Active()) {
            const u64 size = u64{header.size} * sizeof(u32);
            if (split.ListTargetMode() != 0 && g_nx2_ampere.load(std::memory_order_relaxed)) {
                gpu_written = WaitForListWriters(dma_state.dma_get, size,
                                                list_target_verified_live);
            } else if (split.ListPagesMode() != 0) {
                // The range experiment replaces LIST_FAST, including its conservative fallback.
                list_pages_candidate = SkipWriterWaitPages(dma_state.dma_get, size);
                if (!list_pages_candidate) {
                    split.WaitForWriters();
                }
                gpu_written = IsGpuWrittenCommands(dma_state.dma_get, size);
                list_pages_candidate &= !gpu_written;
            } else if (!SkipWriterWait(size)) {
                split.WaitForWriters();
                // The back may have completed between the first query and WaitForWriters.
                gpu_written = IsGpuWrittenCommands(dma_state.dma_get, size);
            }
        }
        // Resume and GPU-written entries need live words, even after a flush cleared dirty state.
        bool submitted = (command_list.snapshot_suspensions & 1) == 0 &&
                               command_list.snapshot_suspensions ==
                                   suspensions.load(std::memory_order_acquire) &&
                               VideoCommon::CanUseSubmittedCommands(
            command_resume_offset != 0,
            dma_pushbuffer_subindex < command_list.submitted_offsets.size() &&
                !std::ranges::binary_search(command_list.deferred_entries,
                                            static_cast<u32>(dma_pushbuffer_subindex)),
            gpu_written,
            use_safe && memory_manager.IsMemoryDirty(dma_state.dma_get,
                                                     header.size * sizeof(u32)));
        bool verified_list = list_target_verified_live;
        if (verified_list) {
            if (submitted) {
                const auto* snapshot = command_list.submitted_words.data() +
                                       command_list.submitted_offsets[dma_pushbuffer_subindex];
                if (std::memcmp(snapshot, command_headers.data(),
                                size_t{header.size} * sizeof(u32)) != 0 &&
                    ++list_target_stats.snapshot_misses <= 16) {
                    LOG_ERROR(HW_GPU,
                              "List target verify: channel={} address={:#x} submitted snapshot "
                              "differs from the post-wait words",
                              channel_state.bind_id, dma_state.dma_get);
                }
            }
            submitted = false; // Verification uses the post-wait words, never an older snapshot.
        }
        if (list_pages_candidate && split.ListPagesMode() == 2) {
            command_headers.resize_destructive(header.size);
            if (submitted) {
                std::memcpy(command_headers.data(), command_list.submitted_words.data() +
                                command_list.submitted_offsets[dma_pushbuffer_subindex],
                            u64{header.size} * sizeof(u32));
            } else {
                memory_manager.ReadBlockUnsafe(dma_state.dma_get, command_headers.data(),
                                               u64{header.size} * sizeof(u32));
            }
            verified_list = VerifyListPages(
                dma_state.dma_get, {command_headers.data(), static_cast<size_t>(header.size)});
            if (!verified_list) {
                submitted = false;
            }
        }
        if (verified_list) {
            const std::span<const CommandHeader> commands{command_headers.data(), header.size};
            RecordParsedEntry(header, commands);
            Nx2CmdPageDiag::CountEntry(submitted ? Nx2CmdPageDiag::Path::Submitted
                                               : Nx2CmdPageDiag::Path::Slow);
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else if (submitted) {
            const std::span<const CommandHeader> commands{
                command_list.submitted_words.data() +
                    command_list.submitted_offsets[dma_pushbuffer_subindex],
                header.size};
            RecordParsedEntry(header, commands);
            Nx2CmdPageDiag::CountEntry(Nx2CmdPageDiag::Path::Submitted);
            command_resume_offset += ProcessCommands(commands, 0);
        } else if (use_safe) {
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::SafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            RecordParsedEntry(header, commands);
            Nx2CmdPageDiag::CountEntry(Nx2CmdPageDiag::Path::Safe);
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else if (ReadGpuWrittenCommands(dma_state.dma_get, static_cast<u32>(header.size))) {
            const std::span<const CommandHeader> commands{command_headers.data(), header.size};
            RecordParsedEntry(header, commands);
            Nx2CmdPageDiag::CountEntry(Nx2CmdPageDiag::Path::Copy);
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else {
            FlushGpuWrittenCommands(dma_state.dma_get, size_t{header.size} * sizeof(u32));
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::UnsafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            RecordParsedEntry(header, commands);
            Nx2CmdPageDiag::CountEntry(Nx2CmdPageDiag::Path::Slow);
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        }
        if (puller.HasPendingAcquire()) {
            return false;
        }
        command_resume_offset = 0;
        if (dma_state.method_count != 0) {
            split_method_end = header.addr + header.size * sizeof(u32);
        }
    }

    if (++dma_pushbuffer_subindex >= command_list_size) {
        // A split method resumes within the submission that split it; ring addresses repeat
        // every frame, so a stale one could otherwise swallow the headers of a later entry.
        for (const auto& parked : parked_methods) {
            static std::atomic<u32> reported{};
            if (reported.fetch_add(1) < 16) {
                LOG_WARNING(HW_GPU,
                            "Submission ended with parked method {:#x} (sub {}, {} words left, "
                            "resume {:#x})",
                            parked.method, parked.subchannel, parked.method_count,
                            parked.resume_address);
            }
        }
        parked_methods.clear();
        dma_pushbuffer.pop_front();
        dma_pushbuffer_subindex = 0;
    } else {
        signal_sync = command_list.command_lists[dma_pushbuffer_subindex].sync && Settings::values.sync_memory_operations.GetValue();
    }

    if (signal_sync) {
        VideoCommon::GpuSplit::Get().Run([this] {
            rasterizer->SignalFence([this]() {
                std::scoped_lock lk(sync_mutex);
                synced = true;
                sync_cv.notify_all();
            });
        }, VideoCommon::DrainSite::GpSyncFence);
    }

    return true;
}

// EDEN_NX2_SEM_WATCH=<gpu address>: log every method whose argument is the low word of an
// address in the 16 bytes there (finds the engine and method that release a semaphore).
static const u32 g_semaphore_watch = [] {
    const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
    return value ? static_cast<u32>(std::strtoull(value, nullptr, 16)) : 0U;
}();

// EDEN_NX2_PB_CHECK also keeps the last parsed entries, for DumpCurrentEntry.
static const bool g_record_parsed_entries = std::getenv("EDEN_NX2_PB_CHECK") != nullptr;

size_t DmaPusher::ProcessCommands(std::span<const CommandHeader> commands, size_t word_offset) {
    for (size_t index = 0; index < commands.size();) {
        // Data word of methods command
        if (dma_state.method_count && dma_state.non_incrementing) {
            auto const& command_header = commands[index]; //must ref (MUltiMethod re)
            dma_state.dma_word_offset = u64((word_offset + index) * sizeof(u32));
            u32 max_write = u32(std::min<std::size_t>(index + dma_state.method_count, commands.size()) - index);
            if (g_nx2_ampere && dma_state.method < non_puller_methods) {
                max_write = 1; // a semaphore acquire may suspend the channel after any word
            }
            CallMultiMethod(&command_header.argument, max_write);
            dma_state.method_count -= max_write;
            dma_state.is_last_call = true;
            index += max_write;
        } else if (dma_state.method_count) {
            // Fast-path batching for consecutive non-execution methods in increasing mode
            if (!dma_state.non_incrementing && !dma_increment_once &&
                dma_state.method >= non_puller_methods &&
                !VideoCommon::GpuSplit::Get().Active() &&
                g_semaphore_watch == 0) {
                auto subchannel = subchannels[dma_state.subchannel];
                if (subchannel != nullptr) {
                    const size_t available = std::min<size_t>(dma_state.method_count, commands.size() - index);
                    u32 batch_count = 0;
                    while (batch_count < available &&
                           (dma_state.method + batch_count) < subchannel->execution_mask.size() &&
                           !subchannel->execution_mask[dma_state.method + batch_count]) {
                        ++batch_count;
                    }
                    if (batch_count > 1) {
                        for (u32 i = 0; i < batch_count; ++i) {
                            subchannel->method_sink.emplace_back(dma_state.method + i, commands[index + i].argument);
                        }
                        dma_state.method += batch_count;
                        dma_state.method_count -= batch_count;
                        index += batch_count;
                        continue;
                    }
                }
            }

            auto const command_header = commands[index]; //can copy
            dma_state.dma_word_offset = u64((word_offset + index) * sizeof(u32));
            dma_state.is_last_call = dma_state.method_count <= 1;
            CallMethod(command_header.argument);
            dma_state.method += !dma_state.non_incrementing ? 1 : 0;
            dma_state.non_incrementing |= dma_increment_once;
            dma_state.method_count--;
            index++;
        } else {
            auto const command_header = commands[index]; //can copy
            // No command active - this is the first word of a new one
            switch (command_header.mode) {
            case SubmissionMode::Increasing:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = false;
                break;
            case SubmissionMode::NonIncreasing:
                SetState(command_header);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::Inline:
                dma_state.method = command_header.method;
                dma_state.subchannel = command_header.subchannel;
                dma_state.dma_word_offset = u64(-s64(dma_state.dma_get)); // negate to set address as 0
                CallMethod(command_header.arg_count);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::IncreaseOnce:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = true;
                break;
            default:
                break;
            }
            index++;
        }
        if (puller.HasPendingAcquire()) {
            return index;
        }
    }
    return commands.size();
}

// NVN on Switch 2 ends GPFIFO entries in the middle of a method's data and calls another command
// buffer (e.g. a compute dispatch) before the entry that carries the rest, which starts right
// where the first one ended. An entry starting anywhere else is such a call: run it with a clean
// method state and resume the unfinished method when the stream continues at that address.
void DmaPusher::ResolveSplitMethod(GPUVAddr entry_address) {
    const auto continued = std::ranges::find(parked_methods, entry_address,
                                             &ParkedMethod::resume_address);
    std::optional<ParkedMethod> resume;
    if (continued != parked_methods.end()) {
        resume = *continued;
        parked_methods.erase(continued);
    }
    if (dma_state.method_count != 0 && entry_address != split_method_end) {
        if (parked_methods.size() == MaxParkedMethods) {
            static std::atomic<u32> reported{};
            if (reported.fetch_add(1) < 8) {
                const auto& oldest = parked_methods.front();
                LOG_WARNING(HW_GPU,
                            "Dropping parked method {:#x} (sub {}, {} words left, resume {:#x})",
                            oldest.method, oldest.subchannel, oldest.method_count,
                            oldest.resume_address);
            }
            parked_methods.erase(parked_methods.begin());
        }
        parked_methods.push_back(ParkedMethod{
            .method = dma_state.method,
            .subchannel = dma_state.subchannel,
            .method_count = dma_state.method_count,
            .non_incrementing = dma_state.non_incrementing,
            .increment_once = dma_increment_once,
            .resume_address = split_method_end,
        });
        dma_state.method_count = 0;
        dma_increment_once = false;
    }
    if (resume && dma_state.method_count == 0) {
        dma_state.method = resume->method;
        dma_state.subchannel = resume->subchannel;
        dma_state.method_count = resume->method_count;
        dma_state.non_incrementing = resume->non_incrementing;
        dma_increment_once = resume->increment_once;
    }
}

void DmaPusher::SetState(const CommandHeader& command_header) {
    dma_state.method = command_header.method;
    dma_state.subchannel = command_header.subchannel;
    dma_state.method_count = command_header.method_count;
}


void DmaPusher::WatchArgument(u32 method, u32 argument) const {
    const u32 watch = g_semaphore_watch;
    if ((argument & ~0xfU) == watch) {
        LOG_INFO(HW_GPU, "Semaphore watch: channel {} subchannel {} method {:#x} argument {:#x}",
                 channel_state.bind_id, dma_state.subchannel, method, argument);
    }
}

bool DmaPusher::WantsSegment() const {
    // Only macro parameters and compute uploads read the engine's current_dma_segment.
    const auto engine = subchannel_type[dma_state.subchannel];
    return engine == Engines::EngineTypes::KeplerCompute ||
           (engine == Engines::EngineTypes::Maxwell3D && dma_state.method >= MacroRegistersStart);
}

void DmaPusher::CallMethod(u32 argument) {
    if (g_semaphore_watch != 0) [[unlikely]] {
        WatchArgument(dma_state.method, argument);
    }
    auto& split = VideoCommon::GpuSplit::Get();
    if (dma_state.method < non_puller_methods) {
        puller.CallPullerMethod(*this, Engines::Puller::MethodCall{
            dma_state.method,
            argument,
            dma_state.subchannel,
            dma_state.method_count,
        });
    } else {
        auto subchannel = subchannels[dma_state.subchannel];
        if (subchannel == nullptr) {
            ReportUnboundSubchannel();
            return;
        }
        const auto engine_type = subchannel_type[dma_state.subchannel];
        if (split.Active() && split.ListTargetMode() == 0 && split.ListPagesMode() != 0 &&
            engine_type == Engines::EngineTypes::MaxwellDMA) {
            static_cast<Engines::MaxwellDMA*>(subchannel)->FrontCallMethod(
                split, dma_state.method, argument, dma_state.is_last_call);
            return;
        }
        const bool writer = split.Active() &&
                            IsMemoryWriter(*subchannel, engine_type, dma_state.method);
        const auto effect = engine_type == Engines::EngineTypes::MaxwellDMA ||
                                    engine_type == Engines::EngineTypes::Fermi2D
                                ? VideoCommon::GpuSplit::Effect::Gpu
                                : VideoCommon::GpuSplit::Effect::Unknown;
        // 2D blits and I2M uploads write guest memory at addresses the front doesn't track: macro
        // DMA reads wait for them (Maxwell3D::FrontDmaRead).
        const bool other_writer = split.Active() &&
                                  (engine_type == Engines::EngineTypes::KeplerMemory ||
                                   engine_type == Engines::EngineTypes::Fermi2D);
        SCOPE_EXIT {
            if (split.Active() && split.ListPagesMode() != 0 &&
                engine_type == Engines::EngineTypes::Maxwell3D) {
                // Even state writes can flush a deferred draw; its shader stores are unbounded.
                split.NoteUnboundedListWriter();
            }
            if (writer) {
                split.NoteWriter();
            } else if (other_writer) {
                split.NoteMemoryWrite(0, ~u64{0}, "2D or I2M");
                split.NoteMemoryWriter();
            }
        };
        if (split.Active()) {
            const GPUVAddr segment = dma_state.dma_get + dma_state.dma_word_offset;
            if (split.FrontMacros()) {
                const auto type = subchannel_type[dma_state.subchannel];
                if (type == Engines::EngineTypes::Maxwell3D) {
                    auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
                    if (maxwell3d->GetNativeMacroEngine().native_turing) {
                        maxwell3d->FrontCallMethod(system, split, dma_state.method, argument,
                                                   dma_state.is_last_call);
                        return;
                    }
                } else if (type == Engines::EngineTypes::KeplerCompute) {
                    auto* const compute = static_cast<Engines::KeplerCompute*>(subchannel);
                    if (compute->FrontRunsMacros()) {
                        compute->FrontCallMethod(system, split, dma_state.method, argument,
                                                 dma_state.is_last_call, &segment);
                        return;
                    }
                }
            }
            split.Method(subchannel, dma_state.method, argument, dma_state.is_last_call,
                         WantsSegment() ? &segment : nullptr, effect);
            return;
        }
        if (!subchannel->execution_mask[dma_state.method]) {
            subchannel->method_sink.emplace_back(dma_state.method, argument);
        } else {
            VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
            subchannel->ConsumeSink(system);
            subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
            subchannel->CallMethod(system, dma_state.method, argument, dma_state.is_last_call);
        }
    }
}

void DmaPusher::CallMultiMethod(const u32* base_start, u32 num_methods) {
    if (g_semaphore_watch != 0) [[unlikely]] {
        for (u32 i = 0; i < num_methods; ++i) {
            WatchArgument(dma_state.method + (dma_state.non_incrementing ? 0 : i), base_start[i]);
        }
    }
    auto& split = VideoCommon::GpuSplit::Get();
    if (dma_state.method < non_puller_methods) {
        puller.CallMultiMethod(*this, dma_state.method, dma_state.subchannel, base_start, num_methods, dma_state.method_count);
    } else {
        auto subchannel = subchannels[dma_state.subchannel];
        if (subchannel == nullptr) {
            ReportUnboundSubchannel();
            return;
        }
        const auto engine_type = subchannel_type[dma_state.subchannel];
        if (split.Active() && split.ListTargetMode() == 0 && split.ListPagesMode() != 0 &&
            engine_type == Engines::EngineTypes::MaxwellDMA) {
            auto* const dma = static_cast<Engines::MaxwellDMA*>(subchannel);
            for (u32 i = 0; i < num_methods; ++i) {
                dma->FrontCallMethod(split, dma_state.method, base_start[i],
                                     dma_state.method_count - i <= 1);
            }
            return;
        }
        const bool writer = split.Active() &&
                            IsMemoryWriter(*subchannel, engine_type, dma_state.method);
        const auto effect = engine_type == Engines::EngineTypes::MaxwellDMA ||
                                    engine_type == Engines::EngineTypes::Fermi2D
                                ? VideoCommon::GpuSplit::Effect::Gpu
                                : VideoCommon::GpuSplit::Effect::Unknown;
        // 2D blits and I2M uploads write guest memory at addresses the front doesn't track: macro
        // DMA reads wait for them (Maxwell3D::FrontDmaRead).
        const bool other_writer = split.Active() &&
                                  (engine_type == Engines::EngineTypes::KeplerMemory ||
                                   engine_type == Engines::EngineTypes::Fermi2D);
        SCOPE_EXIT {
            if (split.Active() && split.ListPagesMode() != 0 &&
                engine_type == Engines::EngineTypes::Maxwell3D) {
                split.NoteUnboundedListWriter();
            }
            if (writer) {
                split.NoteWriter();
            } else if (other_writer) {
                split.NoteMemoryWrite(0, ~u64{0}, "2D or I2M");
                split.NoteMemoryWriter();
            }
        };
        if (split.Active()) {
            const GPUVAddr segment = dma_state.dma_get + dma_state.dma_word_offset;
            if (split.FrontMacros()) {
                const auto type = subchannel_type[dma_state.subchannel];
                if (type == Engines::EngineTypes::Maxwell3D) {
                    auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
                    if (maxwell3d->GetNativeMacroEngine().native_turing) {
                        maxwell3d->FrontCallMultiMethod(system, split, dma_state.method, base_start,
                                                        num_methods, dma_state.method_count);
                        return;
                    }
                } else if (type == Engines::EngineTypes::KeplerCompute) {
                    auto* const compute = static_cast<Engines::KeplerCompute*>(subchannel);
                    if (compute->FrontRunsMacros()) {
                        compute->FrontCallMultiMethod(system, split, dma_state.method, base_start,
                                                      num_methods, dma_state.method_count, &segment);
                        return;
                    }
                }
            }
            split.MultiMethod(subchannel, dma_state.method, base_start, num_methods,
                              dma_state.method_count, WantsSegment() ? &segment : nullptr, effect);
            return;
        }
        VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
        subchannel->ConsumeSink(system);
        subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
        subchannel->CallMultiMethod(system, dma_state.method, base_start, num_methods, dma_state.method_count);
    }
}

void DmaPusher::RefreshSnapshot(CommandList& list, u64 now) {
    list.snapshot_suspensions = now;
    for (size_t i = 0; i < list.command_lists.size() && i < list.submitted_offsets.size(); ++i) {
        const CommandListHeader& entry = list.command_lists[i];
        const u32 offset = list.submitted_offsets[i];
        if (offset + entry.size > list.submitted_words.size()) {
            continue;
        }
        memory_manager.ReadBlockUnsafe(entry.addr, list.submitted_words.data() + offset,
                                       entry.size * sizeof(u32));
    }
    list.deferred_entries.clear();
}

void DmaPusher::RefreshDeferredEntries(CommandList& list) {
    for (const u32 i : list.deferred_entries) {
        if (i >= list.command_lists.size() || i >= list.submitted_offsets.size()) {
            continue;
        }
        const CommandListHeader& entry = list.command_lists[i];
        const u32 offset = list.submitted_offsets[i];
        if (offset + entry.size > list.submitted_words.size()) {
            continue;
        }
        memory_manager.ReadBlockUnsafe(entry.addr, list.submitted_words.data() + offset,
                                       entry.size * sizeof(u32));
    }
    list.deferred_entries.clear();
}

bool DmaPusher::WaitForListWriters(GPUVAddr address, size_t size, bool& verified_live) {
    namespace Profile = VideoCommon::Nx2Profile;
    auto& split = VideoCommon::GpuSplit::Get();
    auto& stats = list_target_stats;
    verified_live = false;
    ++stats.checks;
    SCOPE_EXIT {
        // At each power of two, then every 2^20: Bananza makes ~70,000 checks a second, and every
        // 1024 filled its log with ~1 MB a minute now that it is on by default there.
        if ((stats.checks & (stats.checks - 1)) == 0 || (stats.checks & 0xfffff) == 0) {
            LOG_INFO(HW_GPU,
                     "List target mode={} channel={}: checks={} waits={} unknown={} "
                     "shorter_pending={} fallback={} verified={} changed_entries={} "
                     "changed_words={} became_written={} snapshot_misses={}",
                     split.ListTargetMode(), channel_state.bind_id, stats.checks, stats.waits,
                     stats.unknown_waits, stats.shorter_pending, stats.fallbacks, stats.verified,
                     stats.changed_entries, stats.changed_words, stats.became_written,
                     stats.snapshot_misses);
        }
    };
    const auto fallback = [&] {
        ++stats.fallbacks;
        // Include unknown work even when it follows the last compute/copy writer.
        split.WaitUntil((std::max)(split.LastWriterPosition(), split.Writes().LastUnknown()),
                        VideoCommon::DrainSite::ListWriters);
        return IsGpuWrittenCommands(address, size);
    };
    if (split.HasPendingFunctions()) {
        // Do not translate a list through mappings a queued function may replace.
        split.SettleWrites();
        return fallback();
    }
    const bool sparse = address < g_nx2_sparse_high.load(std::memory_order_relaxed) &&
                        g_nx2_sparse_low.load(std::memory_order_relaxed) < address + size;
    if (size == 0 || address + size < address || sparse) {
        return fallback();
    }
    const u64 consumed = split.RetireWrites();
    u64 target = 0;
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < address + size; page += 0x1000) {
        const auto device = memory_manager.GpuToCpuAddress(page);
        if (!device || (*device & 0xfff) != 0 || *device >= (u64{1} << 36) ||
            memory_manager.GpuToCpuAddress(page + 0xfff) != *device + 0xfff ||
            Nx2IsGpuWritten(*device) || Nx2IsCommandListPage(*device)) {
            return fallback();
        }
        target = (std::max)(target, split.ListTarget(*device, *device + 0x1000));
    }
    const bool unknown = target == split.Writes().LastUnknown();
    stats.shorter_pending += split.LastWriterPosition() > (std::max)(target, consumed) ? 1 : 0;
    const bool waited = split.WaitUntil(target, VideoCommon::DrainSite::ListWriters);
    stats.waits += waited ? 1 : 0;
    stats.unknown_waits += waited && unknown ? 1 : 0;
    if (Profile::Enabled()) {
        Profile::Add(Profile::Counter::ListChecks, 1);
        Profile::Add(Profile::Counter::ListWaits, waited ? 1 : 0);
        Profile::Add(Profile::Counter::ListWaitsUnknown, waited && unknown ? 1 : 0);
    }
    const bool written = IsGpuWrittenCommands(address, size);
    if (split.ListTargetMode() != 2) {
        return written;
    }
    std::vector<u32> early(size / sizeof(u32));
    memory_manager.ReadBlockUnsafe(address, early.data(), size);
    split.WaitForWriters();
    const bool old_written = IsGpuWrittenCommands(address, size);
    command_headers.resize_destructive(early.size());
    memory_manager.ReadBlockUnsafe(address, command_headers.data(), size);
    u64 differences = 0;
    for (size_t i = 0; i < early.size(); ++i) {
        differences += early[i] != command_headers[i].argument ? 1 : 0;
    }
    ++stats.verified;
    stats.changed_words += differences;
    stats.became_written += old_written != written ? 1 : 0;
    if (differences != 0 || old_written != written) {
        if (++stats.changed_entries <= 16) {
            LOG_ERROR(HW_GPU,
                      "List target verify: channel={} address={:#x} words={} changed={} "
                      "gpu_written={}->{} target={} all_writers={}",
                      channel_state.bind_id, address, early.size(), differences, written,
                      old_written, target, split.LastWriterPosition());
        }
    }
    // A GPU-written list still needs ReadGpuWrittenCommands/flush. Otherwise parse the reread.
    verified_live = !old_written;
    return old_written;
}

bool DmaPusher::SkipWriterWaitPages(GPUVAddr address, u64 size) {
    auto& split = VideoCommon::GpuSplit::Get();
    auto& stats = list_pages_stats;
    ++stats.entries;
    SCOPE_EXIT {
        if ((stats.entries & 0x3ff) == 0) {
            LOG_INFO(HW_GPU,
                     "List pages mode={} channel={}: entries={} candidates={} pending={} "
                     "unknown={} mapping={} pages={} overlap={} reports={} checks={} misses={}",
                     split.ListPagesMode(), channel_state.bind_id, stats.entries, stats.candidates,
                     stats.pending_candidates, stats.unknown, stats.mapping, stats.pages,
                     stats.overlap, stats.reports, stats.checks, stats.misses);
        }
    };
    if (stats.misses != 0 || !g_nx2_ampere.load(std::memory_order_relaxed) ||
        split.HasUnboundedListWriter()) {
        ++stats.unknown;
        return false;
    }
    if (size == 0 || address + size < address) {
        ++stats.mapping;
        return false;
    }
    for (GPUVAddr at = address; at < address + size;) {
        const GPUVAddr next = (std::min)(address + size, (at & ~GPUVAddr{0xfff}) + 0x1000);
        const auto device = memory_manager.GpuToCpuAddress(at);
        const u64 bytes = next - at;
        if (!device || (*device & 0xfff) != (at & 0xfff) ||
            *device >= (u64{1} << 36) || bytes > (u64{1} << 36) - *device ||
            memory_manager.GpuToCpuAddress(next - 1) != *device + bytes - 1) {
            ++stats.mapping;
            return false;
        }
        // Query queued ranges before the bits: consumed's acquire observes the back's marks.
        if (split.HasQueuedWritesTo(*device, bytes)) {
            ++stats.overlap;
            return false;
        }
        if (Nx2MayBeGpuWritten(*device, bytes) || Nx2IsCommandListPage(*device)) {
            ++stats.pages;
            return false;
        }
        if (split.IsReportMemory(*device, bytes) || !Nx2AsyncReportsSettled(*device, bytes)) {
            ++stats.reports;
            return false;
        }
        at = next;
    }
    ++stats.candidates;
    stats.pending_candidates += split.HasPendingListWriter() ? 1 : 0;
    return true;
}

bool DmaPusher::VerifyListPages(GPUVAddr address, std::span<const CommandHeader> expected) {
    bool valid = true;
    // Run before parsing. A miss discards the candidate, including a stale submitted snapshot.
    VideoCommon::GpuSplit::Get().Run([memory = &memory_manager, address, expected, &valid] {
        const u64 end = address + expected.size_bytes();
        for (GPUVAddr at = address; at < end;) {
            const GPUVAddr next = (std::min)(end, (at & ~GPUVAddr{0xfff}) + 0x1000);
            const auto device = memory->GpuToCpuAddress(at);
            if (!device || Nx2MayBeGpuWritten(*device, next - at) ||
                Nx2IsCommandListPage(*device) || !Nx2AsyncReportsSettled(*device, next - at)) {
                valid = false;
                return;
            }
            at = next;
        }
        std::vector<CommandHeader> actual(expected.size());
        memory->ReadBlockUnsafe(address, actual.data(), expected.size_bytes());
        valid = std::memcmp(expected.data(), actual.data(), expected.size_bytes()) == 0;
    }, VideoCommon::DrainSite::FrontVerify);
    ++list_pages_stats.checks;
    if (!valid) {
        ++list_pages_stats.misses;
        LOG_WARNING(HW_GPU,
                    "List pages verification FAILED: channel={} address={:#x} bytes={} "
                    "checks={} misses={}; disabling candidates for this channel",
                    channel_state.bind_id, address, expected.size_bytes(), list_pages_stats.checks,
                    list_pages_stats.misses);
    }
    return valid;
}

bool DmaPusher::SkipWriterWait(u64 size) {
    const int mode = VideoCore::Nx2Debug::g_list_fast.load(std::memory_order_relaxed);
    if (mode == 0) {
        return false;
    }
    // The wait only finds a list page a queued dispatch or copy writes for the first time: once
    // no page has been newly written for a while, the set of written pages is taken as settled.
    const u64 marks = Tegra::g_nx2_new_written_pages.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    if (marks != list_fast_marks) {
        list_fast_marks = marks;
        list_fast_since = now;
        return false;
    }
    if (now - list_fast_since < std::chrono::seconds{2}) {
        return false;
    }
    if (mode == 2) {
        const GPUVAddr address = dma_state.dma_get;
        auto* const memory = &memory_manager;
        VideoCommon::GpuSplit::Get().Defer([memory, address, size] {
            static std::atomic<u64> checks{};
            static std::atomic<u64> misses{};
            bool written = false;
            for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < address + size;
                 page += 0x1000) {
                const auto device = memory->GpuToCpuAddress(page);
                written |= device && Tegra::Nx2IsGpuWritten(*device);
            }
            const u64 count = checks.fetch_add(1, std::memory_order_relaxed) + 1;
            if (written && misses.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_WARNING(HW_GPU, "List fast check: entry at {:#x} became GPU-written", address);
            }
            if ((count & 0xffff) == 0) {
                LOG_INFO(HW_GPU, "List fast check: {} entries, {} misses", count,
                         misses.load(std::memory_order_relaxed));
            }
        });
    }
    return true;
}

bool DmaPusher::IsGpuWrittenCommands(GPUVAddr address, size_t size) {
    if (!Tegra::g_nx2_ampere.load(std::memory_order_relaxed) || size == 0) {
        return false;
    }
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < address + size; page += 0x1000) {
        if (const auto device = memory_manager.GpuToCpuAddress(page);
            device && Tegra::Nx2IsGpuWritten(*device)) {
            return true;
        }
    }
    return false;
}

bool DmaPusher::ReadGpuWrittenCommands(GPUVAddr address, u32 count) {
    // The pages are marked as command list pages: from the next write on, the buffer cache copies
    // them to host memory right after the dispatch or copy that writes them and submits. The
    // front then waits only for the back to run that work and for the GPU to finish the copy, not
    // for everything queued and a cache flush (FlushGpuWrittenCommands: ~400 GPU round trips a
    // second in a Mario Kart World race, during which the back had nothing to do). Pages
    // FlushGpuWrittenCommands flushed whole and no GPU work wrote since are read from guest
    // memory.
    if (!Tegra::g_nx2_ampere.load(std::memory_order_relaxed) || count == 0 ||
        rasterizer == nullptr) {
        return false;
    }
    const GPUVAddr end = address + u64{count} * sizeof(u32);
    bool written = false;
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < end; page += 0x1000) {
        if (const auto device = memory_manager.GpuToCpuAddress(page);
            device && Tegra::Nx2IsGpuWritten(*device)) {
            Tegra::Nx2MarkCommandListPage(*device);
            written = true;
        }
    }
    if (!written) {
        return false;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    if (split.Active()) {
        split.WaitForWriters();
    }
    bool past_sync = !split.Active();
    using GpuWrittenList = VideoCore::RasterizerInterface::GpuWrittenList;
    command_headers.resize_destructive(count);
    u8* const out = reinterpret_cast<u8*>(command_headers.data());
    for (GPUVAddr chunk = address; chunk < end;) {
        const GPUVAddr chunk_end = (std::min)(end, (chunk & ~GPUVAddr{0xfff}) + 0x1000);
        const std::span<u8> part{out + (chunk - address), static_cast<size_t>(chunk_end - chunk)};
        const auto device = memory_manager.GpuToCpuAddress(chunk);
        if (!device) {
            return false;
        }
        GpuWrittenList where = GpuWrittenList::Synced;
        if (Tegra::Nx2IsGpuWritten(*device)) {
            where = rasterizer->ReadGpuWrittenCommandList(*device, part, past_sync);
            if (where == GpuWrittenList::NeedsSync) {
                const auto wait_start = Nx2CmdPageDiag::g_enabled
                                            ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
                const bool waited = split.WaitForSync();
                if (Nx2CmdPageDiag::g_enabled && waited) {
                    Nx2CmdPageDiag::NoteSyncWait(std::chrono::steady_clock::now() - wait_start);
                }
                past_sync = true;
                where = rasterizer->ReadGpuWrittenCommandList(*device, part, true);
            }
            if (Nx2CmdPageDiag::g_enabled) {
                auto& diag = Nx2CmdPageDiag::Get();
                const u64 page = *device & ~u64{0xfff};
                if (where == GpuWrittenList::Copied) {
                    diag.reads_copied.fetch_add(1, std::memory_order_relaxed);
                } else if (where == GpuWrittenList::Synced) {
                    diag.reads_synced.fetch_add(1, std::memory_order_relaxed);
                    if (split.Active() && Nx2CmdPageDiag::IsDrawPage(page)) {
                        // The back, at this list's place: did work sent before it write the page?
                        split.Defer([page, begin = static_cast<u32>(*device - page),
                                     end = static_cast<u32>(*device - page + part.size())] {
                            Nx2CmdPageDiag::CheckMissedWrite(page, begin, end);
                        });
                    }
                } else {
                    diag.reads_unknown.fetch_add(1, std::memory_order_relaxed);
                    if (Nx2CmdPageDiag::IsDrawPage(page)) {
                        diag.reads_unknown_draw.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
        if (where == GpuWrittenList::Unknown) {
            return false;
        }
        if (where == GpuWrittenList::Synced) {
            memory_manager.ReadBlockUnsafe(chunk, part.data(), part.size());
        }
        chunk = chunk_end;
    }
    return true;
}

bool DmaPusher::FlushGpuWrittenCommands(GPUVAddr address, size_t size) {
    if (!g_nx2_ampere.load(std::memory_order_relaxed) || size == 0) {
        return false;
    }
    if (!IsGpuWrittenCommands(address, size)) {
        return false;
    }
    namespace SyncProfile = VideoCommon::Nx2SyncProfile;
    SyncProfile::Report();
    SyncProfile::Add(SyncProfile::Counter::ListFallbacks);
    // The caches belong to the split's back: it first runs what the front queued (the dispatch
    // that writes these words among it). The whole pages are flushed: their next lists read
    // guest memory until GPU work writes them again.
    const bool diag = Nx2CmdPageDiag::g_enabled;
    const auto diag_start =
        diag ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::vector<u32> diag_before;
    if (diag) {
        diag_before.resize(size / sizeof(u32));
        memory_manager.ReadBlockUnsafe(address, diag_before.data(), diag_before.size() * sizeof(u32));
    }
    {
        SyncProfile::Scope scope{SyncProfile::Timer::ListPreDrain};
        VideoCommon::GpuSplit::Get().Drain(VideoCommon::DrainSite::ListFlush);
    }
    const GPUVAddr first_page = address & ~GPUVAddr{0xfff};
    const GPUVAddr end_page = (address + size + 0xfff) & ~GPUVAddr{0xfff};
    const bool dirty = memory_manager.IsMemoryDirty(first_page, end_page - first_page,
                                                    VideoCommon::CacheType::BufferCache);
    if (rasterizer != nullptr) {
        std::vector<DAddr> pages;
        for (GPUVAddr page = first_page; page < end_page; page += 0x1000) {
            if (const auto device = memory_manager.GpuToCpuAddress(page)) {
                pages.push_back(*device);
            }
        }
        SyncProfile::Run(
            [rasterizer = rasterizer, pages = std::span<const DAddr>{pages}] {
                rasterizer->FlushGpuWrittenCommandListPages(pages);
            },
            VideoCommon::DrainSite::ListFlush, SyncProfile::Timer::ListRun,
            SyncProfile::Timer::ListPostDrain, SyncProfile::Timer::ListQueue,
            SyncProfile::Timer::ListCallback);
    } else if (dirty) {
        SyncProfile::Run(
            [memory = &memory_manager, first_page, size = end_page - first_page] {
                memory->FlushRegion(first_page, size, VideoCommon::CacheType::BufferCache);
            },
            VideoCommon::DrainSite::ListFlush, SyncProfile::Timer::ListRun,
            SyncProfile::Timer::ListPostDrain, SyncProfile::Timer::ListQueue,
            SyncProfile::Timer::ListCallback);
    }
    if (diag) {
        auto kind = Nx2CmdPageDiag::Kind::NoWrite;
        Nx2CmdPageDiag::PageView first_view;
        u64 first_device{};
        for (GPUVAddr page = first_page; page < end_page; page += 0x1000) {
            const auto device = memory_manager.GpuToCpuAddress(page);
            if (!device) {
                continue;
            }
            const u32 begin = static_cast<u32>((std::max)(address, page) - page);
            const u32 end = static_cast<u32>((std::min)(address + size, page + 0x1000) - page);
            const auto view = Nx2CmdPageDiag::TakePage(*device, begin, end);
            if (page == first_page) {
                first_view = view;
                first_device = *device;
            }
            kind = (std::max)(kind, view.kind);
        }
        std::vector<u32> after(diag_before.size());
        memory_manager.ReadBlockUnsafe(address, after.data(), after.size() * sizeof(u32));
        u64 changed{};
        for (size_t i = 0; i < after.size(); ++i) {
            changed += after[i] != diag_before[i] ? 1 : 0;
        }
        const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - diag_start)
                                            .count());
        Nx2CmdPageDiag::RecordSlow(kind, changed, dirty, ns, address, first_device,
                                   static_cast<u32>(address - first_page),
                                   static_cast<u32>((std::min)(address + size, first_page + 0x1000) -
                                                    first_page),
                                   first_view);
    }
    if (rasterizer != nullptr) {
        for (GPUVAddr page = first_page; page < end_page; page += 0x1000) {
            if (const auto device = memory_manager.GpuToCpuAddress(page);
                device && Tegra::Nx2IsCommandListPage(*device)) {
                rasterizer->NoteGpuWrittenCommandListSynced(*device);
            }
        }
    }
    return true;
}

void DmaPusher::ReportUnboundSubchannel() {
    // Methods sent to a subchannel with no engine (no SetObject yet) are dropped.
    static std::atomic<u32> reported{};
    if (reported.fetch_add(1) < 8) {
        LOG_WARNING(HW_GPU, "Dropping method {:#x} on unbound subchannel {}", dma_state.method,
                    dma_state.subchannel);
    }
}

void DmaPusher::CheckSubmitHash(const CommandListHeader& header, u64 submit_hash) {
    std::vector<u32> words(header.size);
    memory_manager.ReadBlockUnsafe(header.addr, words.data(), words.size() * sizeof(u32));
    static std::atomic<u64> checked{};
    static std::atomic<u64> changed{};
    ++checked;
    if (HashPushbufferWords(words) != submit_hash) {
        if (changed.fetch_add(1) < 16) {
            LOG_WARNING(HW_GPU, "pushbuffer entry {:#x}+{:#x} changed between submit and execution",
                        header.addr.Value(), header.size.Value());
        }
    }
    if ((checked % 20000) == 0) {
        LOG_WARNING(HW_GPU, "pushbuffer check: {} of {} entries changed after submit",
                    changed.load(), checked.load());
    }
}

void DmaPusher::RecordParsedEntry(const CommandListHeader& header,
                                  std::span<const CommandHeader> commands) {
    if (!g_record_parsed_entries) {
        return;
    }
    u64 hash = 0xcbf29ce484222325ULL;
    for (const CommandHeader& command : commands) {
        hash = (hash ^ command.argument) * 0x100000001b3ULL;
    }
    parsed_entries[parsed_entry_count++ % parsed_entries.size()] = {header.addr, static_cast<u32>(header.size), hash};
}

void DmaPusher::DumpCurrentEntry(const char* reason) {
    // Entries whose memory changed after they were parsed (the CPU rewrote them meanwhile).
    for (size_t i = 0; i < std::min(parsed_entry_count, parsed_entries.size()); ++i) {
        const auto& entry = parsed_entries[(parsed_entry_count - 1 - i) % parsed_entries.size()];
        std::vector<u32> words(entry.size);
        memory_manager.ReadBlockUnsafe(entry.address, words.data(), words.size() * sizeof(u32));
        u64 hash = 0xcbf29ce484222325ULL;
        for (const u32 word : words) {
            hash = (hash ^ word) * 0x100000001b3ULL;
        }
        LOG_WARNING(HW_GPU, "  parsed -{}: {:#x}+{:#x} {}", i, entry.address, entry.size,
                    hash == entry.hash ? "unchanged" : "CHANGED since parsed");
    }
    // EDEN_NX2_PB_DUMP=<file>: also write the whole command list (u64 address, u32 size and the
    // current words of every entry, then the current entry index) for offline replay.
    if (static const char* const path = std::getenv("EDEN_NX2_PB_DUMP");
        path != nullptr && !dma_pushbuffer.empty()) {
        if (std::FILE* file = std::fopen(path, "wb")) {
            const auto& lists = dma_pushbuffer.front().command_lists;
            const u64 current = dma_pushbuffer_subindex;
            const u64 count = lists.size();
            std::fwrite(&count, sizeof(count), 1, file);
            std::fwrite(&current, sizeof(current), 1, file);
            for (const CommandListHeader& header : lists) {
                const u64 address = header.addr;
                const u32 size = static_cast<u32>(header.size);
                std::vector<u32> words(size);
                memory_manager.ReadBlockUnsafe(address, words.data(), words.size() * sizeof(u32));
                std::fwrite(&address, sizeof(address), 1, file);
                std::fwrite(&size, sizeof(size), 1, file);
                std::fwrite(words.data(), sizeof(u32), words.size(), file);
            }
            std::fclose(file);
        }
    }
    LOG_WARNING(HW_GPU, "pushbuffer dump ({}): method={:#x} sub={} count={} non_inc={} get={:#x}",
                reason, dma_state.method, dma_state.subchannel, dma_state.method_count,
                dma_state.non_incrementing, dma_state.dma_get);
    if (dma_pushbuffer.empty()) {
        return;
    }
    const auto& lists = dma_pushbuffer.front().command_lists;
    const size_t first = dma_pushbuffer_subindex >= 2 ? dma_pushbuffer_subindex - 2 : 0;
    for (size_t i = first; i <= dma_pushbuffer_subindex && i < lists.size(); ++i) {
        const CommandListHeader& header = lists[i];
        LOG_WARNING(HW_GPU, "  entry {} of {}: addr={:#x} size={:#x}", i, lists.size(),
                    header.addr.Value(), header.size.Value());
        for (u64 word = 0; word < std::min<u64>(header.size, 0x80); word += 8) {
            std::string gpu_line;
            std::string cpu_line;
            for (u64 j = word; j < std::min<u64>(header.size, word + 8); ++j) {
                const GPUVAddr address = header.addr + j * sizeof(u32);
                const u32 gpu_value = memory_manager.Read<u32>(address);
                const u32 cpu_value = system.ApplicationMemory().Read32(address);
                gpu_line += fmt::format(" {:08x}", gpu_value);
                cpu_line += gpu_value == cpu_value ? std::string(" ........")
                                                   : fmt::format(" {:08x}", cpu_value);
            }
            LOG_WARNING(HW_GPU, "    +{:03x}:{}", word * 4, gpu_line);
            LOG_WARNING(HW_GPU, "    cpu :{}", cpu_line);
        }
    }
}

void DmaPusher::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

} // namespace Tegra
