// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "common/logging.h"

namespace VideoCommon::CpuReadbackTrace {

using Clock = std::chrono::steady_clock;
enum Phase : size_t { Lookup, Queue, Invalidate, Flush, Resume, Total,
                      Texture, Buffer, Query, FinishSubmit, FinishWait, Count };
using Sample = std::array<double, Count>;
inline thread_local Sample* active_sample{};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NOVAS_READBACK_TRACE");
        return value && value[0] == '1';
    }();
    return enabled;
}

inline double Milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

class ActiveSample {
public:
    explicit ActiveSample(Sample* sample) : previous{std::exchange(active_sample, sample)} {}
    ~ActiveSample() { active_sample = previous; }
    ActiveSample(const ActiveSample&) = delete;
    ActiveSample& operator=(const ActiveSample&) = delete;

private:
    Sample* previous;
};

class Scope {
public:
    explicit Scope(Phase phase_) : sample{active_sample}, phase{phase_} {
        if (sample) {
            begin = Clock::now();
        }
    }
    ~Scope() {
        if (sample) {
            (*sample)[phase] += Milliseconds(begin, Clock::now());
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Sample* sample;
    Phase phase;
    Clock::time_point begin{};
};

// Completed calls only. Nested phase totals overlap and concurrent CPU readers can overlap too.
inline void Report(const Sample& sample, bool synchronized) {
    static std::mutex mutex;
    static auto last = Clock::now();
    static Sample sums{}, maxima{};
    static size_t calls{}, syncs{};
    std::unique_lock lock{mutex};
    ++calls;
    syncs += synchronized;
    for (size_t i = 0; i < Count; ++i) {
        sums[i] += sample[i];
        maxima[i] = (std::max)(maxima[i], sample[i]);
    }
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - last).count();
    if (seconds < 10.0) {
        return;
    }
    const auto report_sums = std::exchange(sums, Sample{});
    const auto report_maxima = std::exchange(maxima, Sample{});
    const auto report_calls = std::exchange(calls, size_t{});
    const auto report_syncs = std::exchange(syncs, size_t{});
    last = now;
    lock.unlock();
    constexpr std::array names{"lookup", "queue", "invalidate", "flush", "resume", "total",
                               "texture", "buffer", "query", "finish_submit", "finish_wait"};
    std::string phases;
    for (size_t i = 0; i < Count; ++i) {
        phases += fmt::format(" {}={:.1f}/{:.1f}", names[i], report_sums[i], report_maxima[i]);
    }
    LOG_INFO(HW_GPU,
             "NovaS CPU readback over {:.3f} s: {} calls, {} synchronized; "
             "wall ms sum/max (nested phases overlap):{}", seconds, report_calls, report_syncs,
             phases);
}

} // namespace VideoCommon::CpuReadbackTrace
