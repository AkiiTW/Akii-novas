// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <string>

#include "video_core/nx2_profile.h"

namespace VideoCommon::Nx2SyncProfile {

enum class Counter : size_t {
    ListFallbacks,
    ListBatches,
    RequestedNewPages,
    RequestedReusedPages,
    RequestedUntrackedPages,
    ExtraDrawnPages,
    DownloadsNonempty,
    DownloadCopies,
    DownloadBytes,
    Count,
};

enum class Timer : size_t {
    ListPreDrain,
    ListRun,
    ListPostDrain,
    ListQueue,
    ListCallback,
    FenceRun,
    FenceDrain,
    FenceQueue,
    FenceCallback,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Counter::Count)> counter_names{
    "list_fallbacks", "list_batches", "requested_new_pages", "requested_reused_pages",
    "requested_untracked_pages", "extra_drawn_pages", "downloads_nonempty", "download_copies",
    "download_bytes",
};
inline constexpr std::array<const char*, static_cast<size_t>(Timer::Count)> timer_names{
    "list_pre_drain", "list_run", "list_post_drain", "list_queue", "list_callback",
    "fence_run", "fence_drain", "fence_queue", "fence_callback",
};

struct Timing {
    u64 ns{};
    u64 calls{};
    u64 max_ns{};
};

struct ThreadData {
    u64 window_start{};
    std::array<u64, static_cast<size_t>(Counter::Count)> counters{};
    std::array<Timing, static_cast<size_t>(Timer::Count)> timers{};
};

inline thread_local ThreadData t_data;
inline thread_local bool t_list_download{};

inline void Add(Counter counter, u64 amount = 1) {
    if (Nx2Profile::Enabled()) {
        t_data.counters[static_cast<size_t>(counter)] += amount;
    }
}

inline void AddTime(Timer timer, u64 ns) {
    auto& timing = t_data.timers[static_cast<size_t>(timer)];
    timing.ns += ns;
    ++timing.calls;
    timing.max_ns = (std::max)(timing.max_ns, ns);
}

// Independent ten-second windows. These inclusive timings overlap the existing profile zones.
inline void Report() {
    if (!Nx2Profile::Enabled()) {
        return;
    }
    const u64 now = Nx2Profile::Now();
    if (t_data.window_start == 0) {
        t_data.window_start = now;
        return;
    }
    const u64 window = now - t_data.window_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    std::string line;
    for (size_t i = 0; i < counter_names.size(); ++i) {
        line += fmt::format(" {}={}", counter_names[i], t_data.counters[i]);
    }
    for (size_t i = 0; i < timer_names.size(); ++i) {
        const auto& timing = t_data.timers[i];
        line += fmt::format(" {}_ms_per_s={:.3f} {}_calls={} {}_max_ms={:.3f}",
                            timer_names[i], static_cast<double>(timing.ns) * 1000.0 / window,
                            timer_names[i], timing.calls, timer_names[i],
                            static_cast<double>(timing.max_ns) / 1e6);
    }
    LOG_INFO(HW_GPU, "DKB sync thread={} window_wall_ms={:.3f}{}",
             GpuSplit::OnBackThread() ? "back" : "front",
             static_cast<double>(window) / 1e6, line);
    t_data = {};
    t_data.window_start = now;
}

class Scope {
public:
    explicit Scope(Timer timer_) : timer{timer_}, enabled{Nx2Profile::Enabled()} {
        if (enabled) {
            start = Nx2Profile::Now();
        }
    }
    ~Scope() {
        if (enabled) {
            AddTime(timer, Nx2Profile::Now() - start);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Timer timer;
    bool enabled;
    u64 start{};
};

// Run's completion publishes the callback's timestamps to the caller. Use the host wall clock
// across threads, rather than subtracting uncalibrated per-core TSC readings.
template <typename Func>
void Run(const Func& func, DrainSite site, Timer total, Timer drain, Timer queue, Timer callback) {
    auto& split = GpuSplit::Get();
    if (!Nx2Profile::Enabled()) {
        split.Run(func, site);
        return;
    }
    Report();
    u64 begin{};
    u64 end{};
    const u64 start = Nx2Profile::Now();
    const auto timed = [func, &begin, &end] {
        begin = Nx2Profile::Now();
        func();
        end = Nx2Profile::Now();
    };
    // Same dispatch and completion as GpuSplit::Run, with enqueue outside the drain timer.
    if (GpuSplit::OnBackThread() || !split.Active()) {
        timed();
        AddTime(total, Nx2Profile::Now() - start);
    } else {
        split.Defer(timed);
        const u64 drain_start = Nx2Profile::Now();
        split.Drain(site);
        const u64 finished = Nx2Profile::Now();
        AddTime(drain, finished - drain_start);
        AddTime(total, finished - start);
    }
    AddTime(queue, begin - start);
    AddTime(callback, end - begin);
}

// Only downloads initiated by Nx2FlushListPages count here; ordinary readbacks are excluded.
class ListDownloadScope {
public:
    ListDownloadScope() : enabled{Nx2Profile::Enabled()} {
        if (enabled) {
            previous = t_list_download;
            t_list_download = true;
            copies = t_data.counters[static_cast<size_t>(Counter::DownloadCopies)];
        }
    }
    ~ListDownloadScope() {
        if (enabled) {
            if (t_data.counters[static_cast<size_t>(Counter::DownloadCopies)] != copies) {
                Add(Counter::DownloadsNonempty);
            }
            t_list_download = previous;
        }
    }
    ListDownloadScope(const ListDownloadScope&) = delete;
    ListDownloadScope& operator=(const ListDownloadScope&) = delete;

private:
    bool enabled;
    bool previous{};
    u64 copies{};
};

inline void RecordDownload(u64 size) {
    if (Nx2Profile::Enabled() && t_list_download && size != 0) {
        Add(Counter::DownloadCopies);
        Add(Counter::DownloadBytes, size);
    }
}

} // namespace VideoCommon::Nx2SyncProfile
