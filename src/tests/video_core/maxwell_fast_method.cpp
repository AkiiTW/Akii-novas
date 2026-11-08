// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_draw_experiments.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace VideoCommon {

struct GpuSplitTestAccess {
    static u64 TrackEntry(GpuSplit& split, GpuSplit::Effect effect, bool writer = false) {
        ++split.write_position;
        split.writes.Entry(effect, split.write_position);
        if (writer) {
            split.NoteWriter();
        }
        return split.write_position;
    }

    static void TrackRange(GpuSplit& split, u64 begin, u64 end) {
        split.writes.Write(begin, end, split.write_position);
    }

    static void AdvanceWriter(GpuSplit& split) {
        ++split.write_position;
    }

    static void RetireWriters(GpuSplit& split) {
        split.consumed.store(split.write_position, std::memory_order_release);
    }

    static void Initialize(GpuSplit& split, Core::System& system, u64 position = 0) {
        split.system = &system;
        split.ring = std::make_unique<u64[]>(GpuSplit::RingWords);
        split.write_position = position;
        split.consumed_cache = position;
        split.published_local = position;
        split.publish_every = GpuSplit::RingWords;
    }

    static u64 End(const GpuSplit& split) {
        return split.write_position;
    }

    static void Start(GpuSplit& split, Core::System& system) {
        Initialize(split, system);
        split.active = true;
        split.back_thread = std::jthread([&split](std::stop_token stop) { split.BackLoop(stop); });
    }

    static u64 NearWrap() {
        return GpuSplit::RingWords - 1;
    }

    static u64 Execute(GpuSplit& split, u64 position, u64 available) {
        const size_t index = position & GpuSplit::RingMask;
        const size_t size = split.Execute(
            &split.ring[index], std::min<u64>(available - position, GpuSplit::RingWords - index));
        return position + (size != 0 ? size : GpuSplit::RingWords - index);
    }
};

} // namespace VideoCommon

namespace {

using Tegra::Engines::Maxwell3D;

class ScopedLoopSkip {
public:
    explicit ScopedLoopSkip(int mode)
        : setting{VideoCore::Nx2Debug::FindSwitch("EDEN_NX2_LOOP_SKIP")},
          previous{setting->exchange(mode, std::memory_order_relaxed)} {}
    ~ScopedLoopSkip() {
        setting->store(previous, std::memory_order_relaxed);
    }
    ScopedLoopSkip(const ScopedLoopSkip&) = delete;
    ScopedLoopSkip& operator=(const ScopedLoopSkip&) = delete;

private:
    std::atomic<int>* setting;
    int previous;
};

constexpr u32 CbSize = MAXWELL3D_REG_INDEX(const_buffer.size);
constexpr u32 CbAddressHigh = MAXWELL3D_REG_INDEX(const_buffer.address_high);
constexpr u32 CbAddressLow = MAXWELL3D_REG_INDEX(const_buffer.address_low);
constexpr u32 CbOffset = MAXWELL3D_REG_INDEX(const_buffer.offset);
constexpr u32 CbData = MAXWELL3D_REG_INDEX(const_buffer.buffer);
constexpr u32 DrawBegin = MAXWELL3D_REG_INDEX(draw.begin);
constexpr u32 DrawEnd = MAXWELL3D_REG_INDEX(draw.end);
constexpr u32 ShadowControl = MAXWELL3D_REG_INDEX(shadow_ram_control);

// Executable methods without memory or macro effects a null rasterizer cannot take.
constexpr std::array<u32, 16> executable_methods{
    DrawBegin,
    DrawEnd,
    MAXWELL3D_REG_INDEX(vertex_buffer.first),
    MAXWELL3D_REG_INDEX(vertex_buffer.count),
    MAXWELL3D_REG_INDEX(index_buffer.first),
    MAXWELL3D_REG_INDEX(index_buffer.count),
    MAXWELL3D_REG_INDEX(topology_override),
    MAXWELL3D_REG_INDEX(bind_groups[0].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[1].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[2].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[3].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[4].raw_config),
    MAXWELL3D_REG_INDEX(vertex_array_instance_first),
    MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent),
    Maxwell3D::RootTables::SelectMethod,
    Maxwell3D::RootTables::LoadMethod,
};

struct Fixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    Maxwell3D slow{memory};
    Maxwell3D fast{memory};
    std::vector<u32> state_methods;

    Fixture() {
        slow.fast_maxwell3d = false;
        fast.fast_maxwell3d = true;
        for (auto* engine : {&slow, &fast}) {
            engine->BindRasterizer(&rasterizer);
            VideoCommon::Dirty::SetupDirtyFlags(engine->dirty.tables);
            // Give every register a flag so that a missed or extra write shows.
            for (u32 method = 0; method < Maxwell3D::Regs::NUM_REGS; ++method) {
                if (engine->dirty.tables[1][method] == 0) {
                    engine->dirty.tables[1][method] =
                        static_cast<u8>(VideoCommon::Dirty::LastCommonEntry + method % 61);
                }
            }
            engine->dirty.flags.reset();
            // An unmapped buffer: the upload writes nothing, the offset still advances.
            engine->CallMethod(system, CbAddressHigh, 0x1, true);
            engine->CallMethod(system, CbAddressLow, 0x2000'0000, true);
            engine->CallMethod(system, CbSize, 0x10000, true);
        }
        for (u32 method = 0; method < Maxwell3D::Regs::NUM_REGS; ++method) {
            if (!slow.execution_mask[method] && (method < CbSize || method > CbAddressLow)) {
                state_methods.push_back(method);
            }
        }
    }

    void Call(u32 method, u32 argument, bool is_last) {
        slow.CallMethod(system, method, argument, is_last);
        fast.CallMethod(system, method, argument, is_last);
    }

    void CallMulti(u32 method, const std::vector<u32>& words) {
        const u32 amount = static_cast<u32>(words.size());
        slow.CallMultiMethod(system, method, words.data(), amount, amount);
        fast.CallMultiMethod(system, method, words.data(), amount, amount);
    }

    void Compare() const {
        REQUIRE(fast.regs.reg_array == slow.regs.reg_array);
        REQUIRE(fast.shadow_state.reg_array == slow.shadow_state.reg_array);
        REQUIRE(fast.shadow_state.shadow_ram_control == slow.shadow_state.shadow_ram_control);
        REQUIRE(fast.dirty.flags == slow.dirty.flags);
        const auto& a = fast.draw_manager.draw_state;
        const auto& b = slow.draw_manager.draw_state;
        REQUIRE(a.topology == b.topology);
        REQUIRE(a.draw_mode == b.draw_mode);
        REQUIRE(a.draw_indexed == b.draw_indexed);
        REQUIRE(a.draw_element_count == b.draw_element_count);
        REQUIRE(a.base_index == b.base_index);
        REQUIRE(a.base_instance == b.base_instance);
        REQUIRE(a.instance_count == b.instance_count);
        REQUIRE(a.drawn_instances == b.drawn_instances);
        REQUIRE(std::memcmp(&a.vertex_buffer, &b.vertex_buffer, sizeof(a.vertex_buffer)) == 0);
        REQUIRE(std::memcmp(&a.index_buffer, &b.index_buffer, sizeof(a.index_buffer)) == 0);
        for (size_t stage = 0; stage < fast.state.shader_stages.size(); ++stage) {
            for (size_t slot = 0; slot < Maxwell3D::Regs::MaxConstBuffers; ++slot) {
                const auto& x = fast.state.shader_stages[stage].const_buffers[slot];
                const auto& y = slow.state.shader_stages[stage].const_buffers[slot];
                REQUIRE(x.address == y.address);
                REQUIRE(x.size == y.size);
                REQUIRE(x.enabled == y.enabled);
            }
        }
        REQUIRE(fast.root_tables.tables == slow.root_tables.tables);
        REQUIRE(fast.root_tables.selected == slow.root_tables.selected);
        REQUIRE(fast.root_tables.offset == slow.root_tables.offset);
    }
};

u32 RandomArgument(std::mt19937& rng, u32 method, const Maxwell3D& engine) {
    // Mostly values the register already holds or small ones, as the macros write them.
    switch (rng() % 4) {
    case 0:
        return engine.regs.reg_array[method];
    case 1:
        return rng() % 4;
    default:
        break;
    }
    if (method == DrawBegin) {
        // Instance id in bits 26-27: first, subsequent (an instanced loop) or unchanged.
        return (static_cast<u32>(rng() % 3) << 26) | (rng() % 8);
    }
    if (method == Maxwell3D::RootTables::SelectMethod) {
        return (rng() % 8) | ((rng() % 64 * 4) << 8);
    }
    return rng() % 64;
}

} // Anonymous namespace

TEST_CASE("Maxwell3D fast method dispatch matches CallMethod on random method streams",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    for (const u32 seed : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u}) {
        CAPTURE(seed);
        std::mt19937 rng{seed};
        for (u32 step = 0; step < 20000; ++step) {
            CAPTURE(step);
            const u32 kind = rng() % 100;
            if (kind < 45) {
                const u32 method = f.state_methods[rng() % f.state_methods.size()];
                f.Call(method, RandomArgument(rng, method, f.slow), rng() % 2 == 0);
            } else if (kind < 75) {
                const u32 method = executable_methods[rng() % executable_methods.size()];
                f.Call(method, RandomArgument(rng, method, f.slow), rng() % 2 == 0);
            } else if (kind < 85) {
                f.Call(CbData + rng() % 16, rng(), true);
            } else if (kind < 88) {
                f.Call(CbOffset, (rng() % 0x4000) * 4, true);
            } else if (kind < 92) {
                std::vector<u32> words(1 + rng() % 8);
                for (u32& word : words) {
                    word = rng() % 3 == 0 ? rng() : rng() % 4;
                }
                const u32 method = rng() % 2 == 0
                                       ? CbData + rng() % 16
                                       : f.state_methods[rng() % f.state_methods.size()];
                f.CallMulti(method, words);
            } else if (kind < 94) {
                // Track, track with filter, passthrough or replay.
                f.Call(ShadowControl, rng() % 4, true);
            } else if (kind < 97) {
                for (auto* engine : {&f.slow, &f.fast}) {
                    engine->dirty.flags.reset();
                }
            } else {
                for (auto* engine : {&f.slow, &f.fast}) {
                    engine->draw_manager.DrawDeferred(*engine);
                }
            }
            f.Compare();
        }
    }
}

TEST_CASE("Maxwell3D fast method dispatch issues deferred instanced draws as CallMethod does",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    constexpr u32 subsequent = 1u << 26;
    const auto draw = [&f](u32 begin) {
        f.Call(DrawBegin, begin, true);
        f.Call(MAXWELL3D_REG_INDEX(vertex_buffer.first), 0, true);
        f.Call(MAXWELL3D_REG_INDEX(vertex_buffer.count), 3, true);
        f.Call(DrawEnd, 0, true);
        f.Compare();
    };
    draw(0);
    draw(subsequent);
    draw(subsequent);
    REQUIRE(f.fast.draw_manager.HasDeferredDraw());
    // A constant buffer upload, a bind and state changes each end the loop.
    f.Call(CbData, 7, true);
    f.Compare();
    REQUIRE_FALSE(f.fast.draw_manager.HasDeferredDraw());
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(bind_groups[4].raw_config), 1 | (3 << 4), true);
    f.Compare();
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(topology_override), 5, true);
    f.Compare();
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(global_base_instance_index), 9, true);
    f.Compare();
    REQUIRE_FALSE(f.fast.draw_manager.HasDeferredDraw());
}

TEST_CASE("Maxwell3D constant buffer word runs match single CallMethod writes",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    std::mt19937 rng{11};
    constexpr u32 subsequent = 1u << 26;
    for (u32 step = 0; step < 20000; ++step) {
        CAPTURE(step);
        switch (rng() % 8) {
        case 0:
            f.Call(ShadowControl, rng() % 4, true);
            break;
        case 1:
            // Near the end too, where words are dropped.
            f.Call(CbSize, rng() % 2 == 0 ? 0x10000 : 0x100 + rng() % 0x40, true);
            break;
        case 2:
            f.Call(CbOffset, rng() % 3 == 0 ? 0xF0 + (rng() % 8) * 4 : (rng() % 0x400) * 4, true);
            break;
        case 3:
            // An instanced loop the first word ends.
            f.Call(DrawBegin, rng() % 2 == 0 ? 0 : subsequent, true);
            f.Call(DrawEnd, 0, true);
            break;
        default: {
            const u32 count = 1 + rng() % 64;
            std::vector<u32> methods(count);
            std::vector<u32> arguments(count);
            const u32 first = CbData + rng() % 16;
            for (u32 i = 0; i < count; ++i) {
                methods[i] = rng() % 2 == 0 ? first : CbData + (first - CbData + i) % 16;
                arguments[i] = rng() % 2 == 0 ? rng() : rng() % 4;
            }
            for (u32 i = 0; i < count; ++i) {
                f.slow.CallMethod(f.system, methods[i], arguments[i], true);
            }
            f.fast.CallConstBufferWords(f.system, methods.data(), arguments.data(), count);
            break;
        }
        }
        f.Compare();
    }
}

TEST_CASE("GpuSplit CB batching respects device remaps within GPU big pages",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    auto* replacement = f.physical.GetPointerFromRaw<u8>(0x30000);
    for (u64 page = 0; page < 16; ++page) {
        table.entries.GetAndFault(4 + page)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing) - 0x4000);
    }
    table.entries.GetAndFault(0x20).Store(false, Common::PageType::Memory, 1,
                                        reinterpret_cast<uintptr_t>(replacement) - 0x20000);
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(0x10000, 0x10000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(0x10000, 0x4000, 0x10000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(0x100000, 0x10000, 0x10000, Tegra::PTEKind::PITCH, true);
    f.device_memory.Unmap(0x11000, 0x1000);
    f.device_memory.Map(0x11000, 0x20000, 0x1000, asid);
    REQUIRE(f.memory.GetPointer(0x100ffc) == backing + 0xffc);
    REQUIRE(f.memory.GetPointer(0x101000) == replacement);

    for (auto* engine : {&f.slow, &f.fast}) {
        std::memset(backing, 0, 0x10000);
        std::memset(replacement, 0, 0x1000);
        VideoCommon::GpuSplit split;
        Access::Initialize(split, f.system);
        split.MacroWrite(engine, CbAddressHigh, 0);
        split.MacroWrite(engine, CbAddressLow, 0x100ffc);
        split.MacroWrite(engine, CbSize, 8);
        split.MacroWrite(engine, CbOffset, 0);
        split.MacroWrite(engine, CbData, 0x12345678);
        split.MacroWrite(engine, CbData + 1, 0x87654321);
        u64 position{};
        while (position < Access::End(split)) {
            position = Access::Execute(split, position, Access::End(split));
        }
        u32 first{}, second{}, stale{};
        std::memcpy(&first, backing + 0xffc, sizeof(first));
        std::memcpy(&second, replacement, sizeof(second));
        std::memcpy(&stale, backing + 0x1000, sizeof(stale));
        CAPTURE(engine == &f.fast);
        CAPTURE(stale);
        REQUIRE(first == 0x12345678);
        REQUIRE(second == 0x87654321);
        REQUIRE(stale == 0);
        REQUIRE(engine->regs.const_buffer.offset == 8);
    }
    f.Compare();
}

TEST_CASE("GpuSplit dispatch preserves mapped uploads and macro boundaries",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    constexpr GPUVAddr gpu_address = 0x100000;
    constexpr DAddr device_address = 0x10000;
    for (u64 page = 0; page < 3; ++page) {
        const VAddr virtual_address = 0x4000 + page * 0x1000;
        auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000 + page * 0x8000);
        table.entries.GetAndFault(virtual_address >> 12)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing) - virtual_address);
    }
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(device_address, 0x3000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(device_address, 0x4000, 0x3000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(gpu_address, device_address, 0x3000, Tegra::PTEKind::PITCH, false);

    struct Snapshot {
        std::array<u8, 0x3000> bytes{};
        std::array<u32, Maxwell3D::Regs::NUM_REGS> regs{};
        std::array<u32, Maxwell3D::Regs::NUM_REGS> shadow{};
        Maxwell3D::DirtyState::Flags dirty{};
        GPUVAddr segment{};
    };
    const auto snapshot = [&](const Maxwell3D& engine) {
        Snapshot result;
        f.memory.ReadBlockUnsafe(gpu_address, result.bytes.data(), result.bytes.size());
        result.regs = engine.regs.reg_array;
        result.shadow = engine.shadow_state.reg_array;
        result.dirty = engine.dirty.flags;
        result.segment = engine.current_dma_segment;
        return result;
    };

    for (const bool wrap : {false, true}) {
        CAPTURE(wrap);
        std::vector<Snapshot> expected;
        for (auto* engine : {&f.slow, &f.fast}) {
            VideoCommon::GpuSplit split;
            struct ProbeEngine final : Tegra::Engines::EngineInterface {
                u32 writes{};
                void CallMethod(Core::System&, u32, u32, bool) override {
                    ++writes;
                }
                void CallMultiMethod(Core::System&, u32, const u32*, u32 amount, u32) override {
                    writes += amount;
                }
            } probe;
            const u64 start = wrap ? Access::NearWrap() : 0;
            Access::Initialize(split, f.system, start);
            std::vector<u64> boundaries;
            std::array<u8, 0x3000> cleared{};
            f.memory.WriteBlockUnsafe(gpu_address, cleared.data(), cleared.size());
            engine->current_dma_segment = 0;
            engine->dirty.flags.reset();
            const auto state = [&](u32 method, u32 value) {
                split.Method(engine, method, value, true, nullptr);
                boundaries.push_back(Access::End(split));
            };
            const auto direct = [&](u32 method, u32 value) {
                split.MacroWrite(engine, method, value);
                boundaries.push_back(Access::End(split));
            };
            size_t observed{};
            const auto inspect = [&] {
                split.Defer([&, engine] {
                    auto actual = snapshot(*engine);
                    if (engine == &f.slow) {
                        expected.push_back(std::move(actual));
                    } else {
                        REQUIRE(observed < expected.size());
                        const auto& reference = expected[observed];
                        REQUIRE(actual.bytes == reference.bytes);
                        REQUIRE(actual.regs == reference.regs);
                        REQUIRE(actual.shadow == reference.shadow);
                        REQUIRE(actual.dirty == reference.dirty);
                        REQUIRE(actual.segment == reference.segment);
                    }
                    ++observed;
                });
                boundaries.push_back(Access::End(split));
            };
            state(ShadowControl, 0);
            state(CbAddressHigh, 0);
            state(CbAddressLow, static_cast<u32>(gpu_address));
            state(CbSize, 0x3000);
            state(CbOffset, 0xffb);
            direct(MAXWELL3D_REG_INDEX(global_base_instance_index), 17);
            inspect();
            for (u32 i = 0; i < 129; ++i) {
                if (i % 2 == 0) {
                    direct(CbData + i % 16, 0x12340000 + i);
                } else {
                    state(CbData + i % 16, 0x56780000 + i);
                }
            }
            inspect();
            state(ShadowControl, 3);
            state(CbOffset, 0x1ffd);
            for (u32 i = 0; i < 33; ++i) {
                direct(CbData + i % 16, 0xdeadbeef);
            }
            inspect();
            state(ShadowControl, 2);
            state(CbOffset, 0x1ffd);
            direct(DrawBegin, 0);
            direct(DrawEnd, 0);
            direct(DrawBegin, 1u << 26);
            direct(DrawEnd, 0);
            direct(CbData, 0x11223344);
            direct(CbData + 1, 0xaabbccdd);
            const GPUVAddr segment = 0x12345678;
            split.Method(engine, CbData + 2, 0x87654321, false, &segment);
            boundaries.push_back(Access::End(split));
            direct(CbData + 3, 0x76543210);
            split.MacroWrite(&probe, 0x10, 42);
            boundaries.push_back(Access::End(split));
            direct(CbData + 4, 0x65432100);
            const std::array<u32, 3> multi{19, 20, 21};
            split.MultiMethod(engine, CbData, multi.data(), 3, 3, &segment);
            boundaries.push_back(Access::End(split));
            inspect();
            state(CbSize, 7);
            state(CbOffset, 0);
            direct(CbData, 0x99aabbcc);
            direct(CbData + 1, 0xddeeff00);
            inspect();

            u64 position = start;
            if (wrap) {
                for (const u64 available : boundaries) {
                    while (position < available) {
                        position = Access::Execute(split, position, available);
                        REQUIRE(position <= available);
                    }
                }
            } else {
                while (position < Access::End(split)) {
                    position = Access::Execute(split, position, Access::End(split));
                    REQUIRE(position <= Access::End(split));
                }
            }
            REQUIRE(observed == expected.size());
            REQUIRE(probe.writes == 1);
        }
        f.Compare();
    }
}

TEST_CASE("GpuSplit CB batching preserves words at the GPU address space end",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    table.entries.GetAndFault(4).Store(false, Common::PageType::Memory, 1,
                                     reinterpret_cast<uintptr_t>(backing) - 0x4000);
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(0x10000, 0x1000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(0x10000, 0x4000, 0x1000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(0xfffff000, 0x10000, 0x1000, Tegra::PTEKind::PITCH, false);
    for (auto* engine : {&f.slow, &f.fast}) {
        std::memset(backing, 0, 0x1000);
        VideoCommon::GpuSplit split;
        Access::Initialize(split, f.system);
        split.MacroWrite(engine, CbAddressHigh, 0);
        split.MacroWrite(engine, CbAddressLow, 0xfffffffcu);
        split.MacroWrite(engine, CbSize, 8);
        split.MacroWrite(engine, CbOffset, 0);
        split.MacroWrite(engine, CbData, 0x12345678);
        split.MacroWrite(engine, CbData + 1, 0x87654321);
        u64 position{};
        while (position < Access::End(split)) {
            position = Access::Execute(split, position, Access::End(split));
        }
        u32 word{};
        std::memcpy(&word, backing + 0xffc, sizeof(word));
        CAPTURE(engine == &f.fast);
        REQUIRE(word == 0x12345678);
        REQUIRE(engine->regs.const_buffer.offset == 8);
    }
    f.Compare();
}

TEST_CASE("Maxwell3D fast method dispatch cost", "[.][maxwell_fast_method_bench]") {
    Fixture f;
    // Per draw, as the back thread sees Ocarina's: state writes the front did not drop, a
    // constant buffer update, three binds and a draw.
    std::vector<std::pair<u32, u32>> stream;
    std::mt19937 rng{1};
    for (u32 draw = 0; draw < 4096; ++draw) {
        for (u32 i = 0; i < 12; ++i) {
            stream.emplace_back(f.state_methods[rng() % 512], rng() % 8);
        }
        stream.emplace_back(CbOffset, 0x640);
        stream.emplace_back(CbData, draw);
        stream.emplace_back(CbData + 1, 0);
        stream.emplace_back(CbData + 2, draw * 3);
        for (const u32 stage : {0u, 2u, 4u}) {
            stream.emplace_back(MAXWELL3D_REG_INDEX(bind_groups[0].raw_config) + stage * 8,
                                1 | (rng() % 8) << 4);
        }
        stream.emplace_back(DrawBegin, 0);
        stream.emplace_back(MAXWELL3D_REG_INDEX(vertex_buffer.first), rng() % 4);
        stream.emplace_back(MAXWELL3D_REG_INDEX(vertex_buffer.count), 3 + rng() % 4);
        stream.emplace_back(DrawEnd, 0);
    }
    const auto run = [&](Maxwell3D& engine) {
        const auto start = std::chrono::steady_clock::now();
        for (u32 round = 0; round < 50; ++round) {
            for (const auto& [method, argument] : stream) {
                engine.CallMethod(f.system, method, argument, true);
            }
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                   .count() /
               (50.0 * static_cast<double>(stream.size()));
    };
    run(f.slow);
    run(f.fast);
    const double slow_ns = run(f.slow);
    const double fast_ns = run(f.fast);
    WARN("CallMethod: " << slow_ns << " ns per method, fast path " << fast_ns << " ns");

    // Three constant buffer words (a draw macro's c[0][0x640..0x648]) one by one or as a run.
    constexpr std::array<u32, 3> methods{CbData, CbData + 1, CbData + 2};
    constexpr std::array<u32, 3> arguments{1, 2, 3};
    const auto words = [&](bool as_run) {
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < 1'000'000; ++i) {
            f.fast.CallMethod(f.system, CbOffset, 0x640, true);
            if (as_run) {
                f.fast.CallConstBufferWords(f.system, methods.data(), arguments.data(), 3);
            } else {
                for (u32 w = 0; w < 3; ++w) {
                    f.fast.CallMethod(f.system, methods[w], arguments[w], true);
                }
            }
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                   .count() /
               1'000'000.0;
    };
    words(false);
    words(true);
    const double single_ns = words(false);
    const double run_ns = words(true);
    WARN("Three constant buffer words: " << single_ns << " ns one by one, " << run_ns
                                         << " ns as a run (unmapped buffer)");
}

TEST_CASE("Full split drains preserve aliased uploads, partial coverage and queued remaps",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    constexpr VAddr cpu_address = 0x4000;
    constexpr DAddr device_address = 0x10000;
    constexpr GPUVAddr gpu_address = 0x100000;
    constexpr GPUVAddr alias = 0x200000;
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    for (u64 page = 0; page < 2; ++page) {
        const VAddr address = cpu_address + page * 0x1000;
        table.entries.GetAndFault(address >> 12)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing + page * 0x1000) - address);
    }
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    f.device_memory.Map(device_address, cpu_address, 0x2000, asid);
    SCOPE_EXIT {
        f.device_memory.Unmap(device_address, 0x2000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(gpu_address, device_address, 0x2000, Tegra::PTEKind::PITCH, false);
    f.memory.Map(alias, device_address, 0x1000, Tegra::PTEKind::PITCH, false);
    std::memset(backing, 0, 0x2000);

    VideoCommon::GpuSplit split;
    Access::Start(split, f.system);
    SCOPE_EXIT { split.Stop(); };
    const auto state = [&](u32 method, u32 value) {
        split.Method(&f.fast, method, value, true, nullptr);
    };
    const auto upload = [&](GPUVAddr address, u32 size, std::vector<u32> words) {
        state(ShadowControl, 0);
        state(CbAddressHigh, static_cast<u32>(address >> 32));
        state(CbAddressLow, static_cast<u32>(address));
        state(CbSize, size);
        state(CbOffset, 0);
        split.MultiMethod(&f.fast, CbData, words.data(), static_cast<u32>(words.size()),
                          static_cast<u32>(words.size()), nullptr);
        std::ranges::fill(words, 0xdeadbeef);
    };

    upload(gpu_address, 16, {1, 2, 3, 4});
    upload(alias + 4, 4, {20, 30});
    upload(gpu_address + 0x1000, 8, {50, 60});
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    std::array<u32, 4> read{};
    f.memory.ReadBlockUnsafe(gpu_address, read.data(), sizeof(read));
    REQUIRE(read == std::array<u32, 4>{1, 20, 3, 4});
    REQUIRE(f.memory.Read<u32>(alias + 4) == 20);
    REQUIRE(f.memory.Read<u32>(gpu_address + 0x1000) == 50);

    split.Defer([&] {
        f.memory.Map(alias, device_address + 0x1000, 0x1000, Tegra::PTEKind::PITCH, false);
    });
    upload(alias, 4, {70});
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    REQUIRE(f.memory.Read<u32>(gpu_address) == 1);
    REQUIRE(f.memory.Read<u32>(gpu_address + 0x1000) == 70);
    REQUIRE(f.memory.Read<u32>(alias + 4) == 60);
}

TEST_CASE("Split consumption does not complete deferred downstream callbacks",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    VideoCommon::GpuSplit split;
    Access::Start(split, f.system);
    SCOPE_EXIT { split.Stop(); };
    std::vector<std::function<void()>> deferred;
    std::array<u32, 3> output{};
    for (u32 i = 0; i < output.size(); ++i) {
        split.Defer([&, i] { deferred.emplace_back([&, i] { output[i] = i + 1; }); });
    }
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    REQUIRE(deferred.size() == 3);
    REQUIRE(output == std::array<u32, 3>{});
    split.Run([&] {
        for (auto& operation : deferred) {
            operation();
        }
        deferred.clear();
    });
    REQUIRE(output == std::array<u32, 3>{1, 2, 3});
}

TEST_CASE("Split restart resets writer positions without replaying old releases",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    VideoCommon::GpuSplit split;
    u32 released{};
    u32 release_count{};
    for (u32 session = 1; session <= 3; ++session) {
        Access::Start(split, f.system);
        SCOPE_EXIT { split.Stop(); };
        REQUIRE(Access::End(split) == 0);
        REQUIRE_FALSE(split.WaitForWriters());
        REQUIRE_FALSE(split.WaitForSync());
        split.Defer([&, session] {
            released = session;
            ++release_count;
        });
        split.NoteWriter();
        const u64 epoch = split.ListTrackingEpoch();
        split.NoteSync();
        split.Drain(VideoCommon::DrainSite::Acquire);
        REQUIRE(released == session);
        REQUIRE(release_count == session);
        split.Stop();
        REQUIRE_FALSE(split.Active());
        REQUIRE_FALSE(split.BackRunning());
        REQUIRE(Access::End(split) == 0);
        REQUIRE(split.ListTrackingEpoch() > epoch);
        REQUIRE_FALSE(split.HasUnboundedListWriter());
        REQUIRE(split.ListTarget(0x1000, 0x2000) == 0);
    }
}

TEST_CASE("List page dependencies retain unknown writers and use half-open device ranges",
          "[video_core][list_pages]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    auto split = std::make_unique<VideoCommon::GpuSplit>();
    Access::AdvanceWriter(*split);
    split->NoteWriter(0x1000, 0x20);
    REQUIRE_FALSE(split->HasUnboundedListWriter());
    REQUIRE(split->HasPendingListWriter());
    REQUIRE(split->HasQueuedWritesTo(0x101f, 1));
    REQUIRE(split->HasQueuedWritesTo(0xfff, 2));
    REQUIRE_FALSE(split->HasQueuedWritesTo(0x1020, 4));
    REQUIRE_FALSE(split->HasQueuedWritesTo(0xffc, 4));

    Access::AdvanceWriter(*split);
    split->NoteWriter();
    Access::AdvanceWriter(*split);
    split->NoteWriter(0x2000, 16);
    REQUIRE(split->HasUnboundedListWriter());
    Access::RetireWriters(*split);
    REQUIRE_FALSE(split->HasUnboundedListWriter());
    REQUIRE_FALSE(split->HasPendingListWriter());
    REQUIRE_FALSE(split->HasQueuedWritesTo(0x1000, 0x2000));
}

TEST_CASE("ListTarget waits for unknown and overlapping writes but only synchronized compute",
          "[video_core][list_target]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    using Effect = VideoCommon::GpuSplit::Effect;
    auto split = std::make_unique<VideoCommon::GpuSplit>();
    const u64 first = Access::TrackEntry(*split, Effect::Gpu, true);
    REQUIRE(split->ListTarget(0x1000, 0x2000) == 0);
    REQUIRE(split->LastWriterPosition() == first);
    split->NoteSync();
    const u64 second = Access::TrackEntry(*split, Effect::Gpu, true);
    REQUIRE(split->ListTarget(0x1000, 0x2000) == first);
    REQUIRE(split->ListTarget(0x1000, 0x2000) < second);

    const u64 unknown = Access::TrackEntry(*split, Effect::Unknown);
    REQUIRE(split->ListTarget(0x1000, 0x2000) == unknown);
    const u64 cb_write = Access::TrackEntry(*split, Effect::None);
    Access::TrackRange(*split, 0x1000, 0x2000);
    REQUIRE(split->ListTarget(0x1000, 0x2000) == cb_write);
    REQUIRE(split->ListTarget(0x2000, 0x3000) == unknown);

    Access::TrackEntry(*split, Effect::Gpu, true);
    split->NoteSync(); // Also used by an acquire that SEM_FAST satisfied without a drain.
    REQUIRE(split->ListTarget(0x8000, 0x9000) == split->LastWriterPosition());
    Access::RetireWriters(*split);
    const u64 consumed = split->RetireWrites();
    REQUIRE(split->Writes().LastWriteTo(0x1000, 0x2000) == 0);
    REQUIRE(split->ListTarget(0x1000, 0x2000) <= consumed);
    REQUIRE_FALSE(split->WaitUntil(split->ListTarget(0x1000, 0x2000),
                                   VideoCommon::DrainSite::ListWriters));
}

TEST_CASE("List page dependency overflow and invalid destinations fail closed",
          "[video_core][list_pages]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    auto split = std::make_unique<VideoCommon::GpuSplit>();
    for (u32 i = 0; i < 8193; ++i) {
        Access::AdvanceWriter(*split);
        split->NoteWriter(0x1000, 16);
    }
    REQUIRE(split->HasQueuedWritesTo(0x8000, 16));
    Access::RetireWriters(*split);
    REQUIRE_FALSE(split->HasQueuedWritesTo(0x8000, 16));
    constexpr std::array<std::pair<u64, u64>, 3> invalid_ranges{{
        {0x1000, 0}, {~u64{0} - 3, 16}, {(u64{1} << 36) - 8, 16}}};
    for (const auto& [begin, size] : invalid_ranges) {
        Access::AdvanceWriter(*split);
        split->NoteWriter(begin, size);
        REQUIRE(split->HasUnboundedListWriter());
        Access::RetireWriters(*split);
    }
}

TEST_CASE("DMA list destinations require complete front state and every mapped page",
          "[video_core][list_pages]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.memory.BindRasterizer(&f.rasterizer);
    constexpr GPUVAddr gpu_address = 0x100000;
    constexpr DAddr device_address = 0x10000;
    f.memory.Map(gpu_address, device_address, 0x3000, Tegra::PTEKind::PITCH, false);
    auto split = std::make_unique<VideoCommon::GpuSplit>();
    Access::Initialize(*split, f.system);
    Tegra::Engines::MaxwellDMA dma{f.memory};
    const auto method = [&](u32 reg, u32 value) { dma.FrontCallMethod(*split, reg, value, true); };
    // LAUNCH_DMA: pitch destination, no semaphore/remap/multiline.
    constexpr u32 launch = 1U << 8;
    method(0xc0, launch);
    REQUIRE(split->HasUnboundedListWriter());
    Access::RetireWriters(*split);
    method(0x102, 0);
    method(0x103, static_cast<u32>(gpu_address));
    method(0x106, 0x3000);
    method(0xc0, launch);
    REQUIRE_FALSE(split->HasUnboundedListWriter());
    REQUIRE(split->HasQueuedWritesTo(device_address + 0x2000, 16));
    REQUIRE_FALSE(split->HasQueuedWritesTo(device_address + 0x3000, 16));
    Access::RetireWriters(*split);

    SECTION("An interior alias is not covered by matching endpoints") {
        f.memory.Map(gpu_address + 0x1000, device_address + 0x5000, 0x1000,
                     Tegra::PTEKind::PITCH, false);
        method(0xc0, launch);
        REQUIRE(split->HasUnboundedListWriter());
    }
    SECTION("Short non-pitch-source copies cannot understate the write extent") {
        method(0x106, 15);
        method(0xc0, launch);
        REQUIRE(split->HasUnboundedListWriter());
    }
    SECTION("A semaphore side effect remains unknown") {
        method(0xc0, launch | (1U << 3));
        REQUIRE(split->HasUnboundedListWriter());
    }
    SECTION("Invalid register indices never enter the back's method sink") {
        const u64 before = Access::End(*split);
        method(0x800, 1);
        REQUIRE(Access::End(*split) == before);
    }
}

TEST_CASE("Maxwell3D takes its macros back from the split's front once the split stops",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    VideoCommon::GpuSplit split;
    Access::Initialize(split, f.system);
    auto& engine = f.fast;
    constexpr u32 StartAddressPointer = MAXWELL3D_REG_INDEX(load_mme.start_address_ptr);
    constexpr u32 StartAddress = MAXWELL3D_REG_INDEX(load_mme.start_address);
    const u32 state = f.state_methods.front();
    // The front takes the macros over and tracks a state write in its shadow RAM copy.
    engine.FrontCallMethod(f.system, split, state, 0x1234, true);
    REQUIRE(engine.shadow_state.reg_array[state] != 0x1234);
    // The split stopped: the next engine method runs on this thread, not the split's back.
    engine.CallMethod(f.system, StartAddressPointer, 0, true);
    engine.CallMethod(f.system, StartAddress, 0x40, true);
    CHECK(engine.shadow_state.reg_array[state] == 0x1234);
    CHECK(engine.GetNativeMacroPositions()[0] == 0x40);
}

TEST_CASE("Split vertex store elision preserves sink and shadow effects",
          "[video_core][dkb_draw_experiments][dkb_vertex_stores]") {
    const ScopedLoopSkip loop_skip{0};
    using Access = VideoCommon::GpuSplitTestAccess;
    using Shadow = Maxwell3D::Regs::ShadowRamControl;
    Fixture f;
    f.system.Initialize();
    VideoCommon::GpuSplit split;
    Access::Start(split, f.system);
    SCOPE_EXIT { split.Stop(); };
    auto& engine = f.fast;
    constexpr u32 first = MAXWELL3D_REG_INDEX(vertex_buffer.first);
    constexpr u32 count = MAXWELL3D_REG_INDEX(vertex_buffer.count);
    const bool enabled = VideoCommon::Nx2DrawExperiments::VertexStores();
    const auto send = [&](u32 method, u32 value) {
        engine.FrontCallMethod(f.system, split, method, value, true);
    };
    const auto complete = [&] { split.Drain(); };
    const auto repeat = [&](u32 method, u32 value, bool can_skip) {
        const u64 before = Access::End(split);
        send(method, value);
        REQUIRE((Access::End(split) == before) == (enabled && can_skip));
        complete();
    };
    send(ShadowControl, static_cast<u32>(Shadow::Track));
    send(first, 3);
    send(count, 12);
    complete();
    repeat(first, 3, true);
    repeat(count, 12, true);

    // Even an unchanged executable must consume a preceding state write before other engines.
    const u32 state = MAXWELL3D_REG_INDEX(global_base_instance_index);
    send(state, 41);
    repeat(count, 12, false);
    REQUIRE(engine.method_sink.empty());
    REQUIRE(engine.regs.reg_array[state] == 41);

    // Method runs also consume the sink. A following unchanged vertex store can be omitted.
    send(state, 42);
    const std::array<u32, 1> values{12};
    engine.FrontCallMultiMethod(f.system, split, count, values.data(), 1, 1);
    repeat(count, 12, true);
    REQUIRE(engine.method_sink.empty());
    REQUIRE(engine.regs.reg_array[state] == 42);

    // Equal register value, different shadow value: the Track write must reach the back.
    send(ShadowControl, static_cast<u32>(Shadow::Passthrough));
    send(first, 7);
    send(ShadowControl, static_cast<u32>(Shadow::Track));
    repeat(first, 7, false);
    REQUIRE(engine.shadow_state.reg_array[first] == 7);
    send(ShadowControl, static_cast<u32>(Shadow::Replay));
    repeat(first, 99, true); // Effective value is the shadow's 7, not the supplied 99.
    REQUIRE(engine.regs.reg_array[first] == 7);

    send(ShadowControl, static_cast<u32>(Shadow::Track));
    complete();
    // These look like stores but carry dirty flags or deferred-draw effects.
    for (const u32 method : {static_cast<u32>(MAXWELL3D_REG_INDEX(index_buffer.first)),
                            static_cast<u32>(MAXWELL3D_REG_INDEX(topology_override))}) {
        const u32 value = engine.regs.reg_array[method];
        send(method, value); // Establish matching shadow and register values.
        complete();
        repeat(method, value, false);
    }
}

TEST_CASE("Loop skip consumes a pending sink before switching engines",
          "[video_core][loop_skip][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    constexpr u32 state = MAXWELL3D_REG_INDEX(global_base_instance_index);
    constexpr u32 count = MAXWELL3D_REG_INDEX(vertex_buffer.count);
    struct Observer final : Tegra::Engines::EngineInterface {
        Maxwell3D* source{};
        bool sink_empty{};
        u32 value{};
        void CallMethod(Core::System&, u32, u32, bool) override {
            sink_empty = source->method_sink.empty();
            value = source->regs.global_base_instance_index;
        }
        void CallMultiMethod(Core::System&, u32, const u32*, u32, u32) override {}
    };
    for (const int mode : {1, 2}) {
        CAPTURE(mode);
        const ScopedLoopSkip loop_skip{mode};
        Fixture f;
        f.system.Initialize();
        VideoCommon::GpuSplit split;
        Observer observer;
        observer.source = &f.fast;
        Access::Start(split, f.system);
        SCOPE_EXIT { split.Stop(); };
        const auto send = [&](u32 method, u32 value) {
            f.fast.FrontCallMethod(f.system, split, method, value, true);
        };
        send(ShadowControl, static_cast<u32>(Maxwell3D::Regs::ShadowRamControl::Track));
        send(count, 12);
        split.Drain();
        const u64 safe_before = Access::End(split);
        send(count, 12);
        REQUIRE((Access::End(split) == safe_before) == (mode == 1));
        split.Drain();
        send(state, 41);
        const u64 before = Access::End(split);
        send(count, 12);
        REQUIRE(Access::End(split) > before);
        split.MacroWrite(&observer, 0x10, 0);
        split.Drain();
        REQUIRE(observer.sink_empty);
        REQUIRE(observer.value == 41);
    }
}

TEST_CASE("Loop skip tracks every effective draw begin in a packet",
          "[video_core][loop_skip][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    using Shadow = Maxwell3D::Regs::ShadowRamControl;
    using DrawMode = Maxwell3D::DrawManager::DrawMode;
    constexpr u32 subsequent = 1U << 26;
    constexpr u32 unchanged = 2U << 26;
    for (const int mode : {1, 2}) {
        CAPTURE(mode);
        const ScopedLoopSkip loop_skip{mode};
        Fixture f;
        f.system.Initialize();
        VideoCommon::GpuSplit split;
        Access::Start(split, f.system);
        SCOPE_EXIT { split.Stop(); };
        const auto send = [&](u32 method, u32 value) {
            f.fast.FrontCallMethod(f.system, split, method, value, true);
        };
        send(ShadowControl, static_cast<u32>(Shadow::Track));
        send(DrawBegin, 0);
        send(DrawEnd, 0);
        send(DrawBegin, subsequent);
        const u64 instance_before = Access::End(split);
        send(DrawEnd, 0);
        REQUIRE((Access::End(split) == instance_before) == (mode == 1));
        const std::array<u32, 2> values{0, unchanged};
        f.fast.FrontCallMultiMethod(f.system, split, DrawBegin, values.data(), 2, 2);
        split.Drain();
        REQUIRE(f.fast.draw_manager.draw_state.draw_mode == DrawMode::General);
        const auto check_end = [&] {
            const u64 before = Access::End(split);
            send(DrawEnd, 0);
            REQUIRE(Access::End(split) > before);
            split.Drain();
        };
        check_end();

        // Replay must use Unchanged from shadow RAM, not Subsequent from the packet.
        send(ShadowControl, static_cast<u32>(Shadow::Replay));
        const std::array<u32, 2> replay_values{subsequent, subsequent};
        f.fast.FrontCallMultiMethod(f.system, split, DrawBegin, replay_values.data(), 2, 2);
        check_end();
        REQUIRE(f.fast.draw_manager.draw_state.draw_mode == DrawMode::General);

        send(ShadowControl, static_cast<u32>(Shadow::Track));
        send(DrawBegin, subsequent);
        const std::array<u32, 1> indices{0};
        f.fast.FrontCallMultiMethod(f.system, split, MAXWELL3D_REG_INDEX(draw_inline_index),
                                   indices.data(), 1, 1);
        check_end();
        if (mode == 2) {
            const auto& counts = f.fast.GetLoopSkipVerification().methods[0];
            REQUIRE(counts.checked == 1);
            REQUIRE(counts.sink_pending == 0);
            REQUIRE(counts.value_mismatch == 0);
            REQUIRE(counts.shadow_mismatch == 0);
            REQUIRE(counts.draw_mode_mismatch == 0);
        }
    }
}

TEST_CASE("Loop skip verifier detects back effects without suppressing writes",
          "[video_core][loop_skip][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    using Effect = VideoCommon::GpuSplit::Effect;
    using Shadow = Maxwell3D::Regs::ShadowRamControl;
    constexpr u32 count = MAXWELL3D_REG_INDEX(vertex_buffer.count);
    constexpr u32 state = MAXWELL3D_REG_INDEX(global_base_instance_index);
    const ScopedLoopSkip loop_skip{2};
    for (const bool fast : {false, true}) {
        CAPTURE(fast);
        Fixture f;
        f.system.Initialize();
        auto& engine = fast ? f.fast : f.slow;
        VideoCommon::GpuSplit split;
        Access::Start(split, f.system);
        SCOPE_EXIT { split.Stop(); };
        split.MacroWrite(&engine, ShadowControl, static_cast<u32>(Shadow::Track));
        split.Method(&engine, state, 42, true, nullptr);
        // Deliberately mark unsafe candidates: the checker must run before ConsumeSink.
        split.Method(&engine, count, 0, true, nullptr, Effect::None, true);
        split.MacroWrite(&engine, count, 7, Effect::None, true);
        split.Method(&engine, DrawEnd, 0, true, nullptr, Effect::None, true);
        split.MacroWrite(&engine, ShadowControl, static_cast<u32>(Shadow::Replay));
        split.MacroWrite(&engine, count, 99, Effect::None, true);
        split.Drain();
        const auto& counts = engine.GetLoopSkipVerification();
        REQUIRE(counts.methods[2].checked == 3);
        REQUIRE(counts.methods[2].sink_pending == 1);
        REQUIRE(counts.methods[2].value_mismatch == 1);
        REQUIRE(counts.methods[2].shadow_mismatch == 1);
        REQUIRE(counts.methods[0].checked == 1);
        REQUIRE(counts.methods[0].draw_mode_mismatch == 1);
        REQUIRE(engine.method_sink.empty());
        REQUIRE(engine.regs.reg_array[state] == 42);
        REQUIRE(engine.regs.reg_array[count] == 7);
        REQUIRE(engine.shadow_state.reg_array[count] == 7);

        split.Defer([&engine, state] {
            engine.method_sink.emplace_back(state, 43);
            auto& draw = engine.draw_manager.draw_state;
            draw.draw_mode = Maxwell3D::DrawManager::DrawMode::Instance;
            draw.instance_count = 1;
            draw.drawn_instances = 3;
            engine.VerifyLoopMacroFinish(true);
        });
        split.Drain();
        REQUIRE(counts.macro_checked == 1);
        REQUIRE(counts.macro_would_skip == 1);
        REQUIRE(counts.macro_sink_pending == 1);
        REQUIRE(counts.macro_deferred_draw == 1);
        REQUIRE(counts.macro_drawn_instances == 1);
    }
}

TEST_CASE("Loop skip verifies macro finish and preserves DrawDeferred resets",
          "[video_core][loop_skip][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    const ScopedLoopSkip loop_skip{2};
    Fixture f;
    f.system.Initialize();
    auto& engine = f.fast;
    auto& macro = engine.GetNativeMacroEngine();
    macro.native_turing = true;
    macro.ClearCode(0);
    // Two TU104 instructions: emit vertex_buffer.count=12, then an empty end delay slot.
    for (const bool emit : {true, false}) {
        std::array<u32, 3> bits{};
        const auto field = [&](u32 offset, u32 size, u32 value) {
            for (u32 bit = 0; bit < size; ++bit) {
                bits[(offset + bit) / 32] |= ((value >> bit) & 1U) << ((offset + bit) % 32);
            }
        };
        for (const u32 offset : {15U, 20U, 25U, 51U, 56U, 61U}) {
            field(offset, 5, 24); // ADD zero + zero, discard both ALU results.
        }
        if (emit) {
            field(0, 1, 1);
            field(30, 16, MAXWELL3D_REG_INDEX(vertex_buffer.count));
            field(66, 16, 12);
            field(82, 3, 5); // Method from immediate 0.
            field(85, 4, 6); // Value from immediate 1.
        }
        for (const u32 word : {bits[2], bits[1], bits[0]}) {
            macro.AddCode(0, word);
        }
    }
    engine.GetNativeMacroPositions()[0] = 0;
    VideoCommon::GpuSplit split;
    Access::Start(split, f.system);
    SCOPE_EXIT { split.Stop(); };
    engine.FrontCallMethod(f.system, split, ShadowControl,
                           static_cast<u32>(Maxwell3D::Regs::ShadowRamControl::Track), true);
    const auto call = [&] {
        engine.FrontCallMethod(f.system, split, 0xe00, 0, true);
        split.Drain();
        REQUIRE_FALSE(macro.native_execution_failed);
    };
    call();
    REQUIRE(engine.regs.vertex_buffer.count == 12);
    call();
    const auto& counts = engine.GetLoopSkipVerification();
    REQUIRE(counts.methods[2].checked == 1);
    REQUIRE(counts.methods[2].sink_pending == 0);
    REQUIRE(counts.methods[2].value_mismatch == 0);
    REQUIRE(counts.methods[2].shadow_mismatch == 0);
    REQUIRE(counts.macro_checked == 2);
    REQUIRE(counts.macro_would_skip == 1);
    REQUIRE(counts.macro_deferred_draw == 0);
    REQUIRE(counts.macro_drawn_instances == 0);
    split.Defer([&engine] { engine.draw_manager.draw_state.drawn_instances = 3; });
    call();
    REQUIRE(counts.macro_would_skip == 2);
    REQUIRE(counts.macro_drawn_instances == 1);
    REQUIRE(engine.draw_manager.draw_state.drawn_instances == 0);
}

TEST_CASE("Loop skip initializes lazily and preserves live overrides including zero",
          "[video_core][loop_skip]") {
    using VideoCore::Nx2Debug::InitializeLoopSkip;
    for (const auto& [text, expected] : std::array<std::pair<const char*, int>, 7>{{
             {nullptr, 0}, {"", 0}, {"0", 0}, {"1", 1}, {"2", 2}, {"3", 0}, {"20", 0}}}) {
        CAPTURE(text, expected);
        std::atomic<int> setting{-1};
        REQUIRE(InitializeLoopSkip(setting, text) == expected);
        REQUIRE(setting.load() == expected);
        REQUIRE(InitializeLoopSkip(setting, "1") == expected);
    }
    const ScopedLoopSkip loop_skip{2};
    auto* setting = VideoCore::Nx2Debug::FindSwitch("EDEN_NX2_LOOP_SKIP");
    REQUIRE(VideoCore::Nx2Debug::LoopSkipMode() == 2);
    setting->store(0);
    REQUIRE(InitializeLoopSkip(*setting, "2") == 0);
    REQUIRE(VideoCore::Nx2Debug::LoopSkipMode() == 0);
    setting->store(1);
    REQUIRE(VideoCore::Nx2Debug::LoopSkipMode() == 1);
}

TEST_CASE("Maxwell3D keeps state written after the split stops over the front's shadow RAM",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    VideoCommon::GpuSplit split;
    Access::Initialize(split, f.system);
    auto& engine = f.fast;
    const u32 state = f.state_methods.front();
    engine.FrontCallMethod(f.system, split, state, 0x1234, true);
    // After the stop, DmaPusher sinks a state write and consumes the sink before the next
    // executable method: the handback must come first so that write wins.
    engine.method_sink.emplace_back(state, 0x5678);
    engine.ConsumeSink(f.system);
    engine.CallMethod(f.system, MAXWELL3D_REG_INDEX(load_mme.start_address_ptr), 0, true);
    CHECK(engine.shadow_state.reg_array[state] == 0x5678);
    CHECK(engine.regs.reg_array[state] == 0x5678);
}
