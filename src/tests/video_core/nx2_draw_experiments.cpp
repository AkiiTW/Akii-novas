// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>
#include <random>
#include <span>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/read_trap_tracker.h"
#include "video_core/buffer_cache/word_manager.h"
#include "video_core/memory_layout_cache.h"

namespace {
constexpr u64 Page = 4096;
constexpr u64 Word = Page * 64;
constexpr VAddr Base = 0x400000;
using Range = std::pair<u64, u64>;

struct Tracker {
    std::vector<std::tuple<VAddr, size_t, int>> notifications;
    void UpdatePagesCachedCount(VAddr address, size_t size, int delta) {
        notifications.emplace_back(address, size, delta);
    }
    void UpdatePagesCachedBatch(std::span<const std::pair<VAddr, size_t>> ranges, int delta) {
        for (const auto& [address, size] : ranges) {
            UpdatePagesCachedCount(address, size, delta);
        }
    }
};
} // namespace

TEST_CASE("Empty tracker words preserve ranges notifications and clean summaries",
          "[video_core][dkb_draw_experiments][dkb_clean_words]") {
    using namespace VideoCommon;
    Tracker reference_tracker, fast_tracker;
    WordManager<Tracker, 4, 4 * Word> reference{Base, reference_tracker};
    WordManager<Tracker, 4, 4 * Word> fast{Base, fast_tracker};
    std::mt19937_64 rng{0x172565989};
    constexpr std::array types{Type::CPU, Type::GPU, Type::CachedCPU, Type::Preflushable};
    for (size_t step = 0; step < 2000; ++step) {
        CAPTURE(step);
        for (auto& bits : reference.heap) {
            bits = rng() % 3 == 0 ? 0 : rng();
        }
        fast.heap = reference.heap;
        for (size_t i = 0; i < reference.num_words; ++i) {
            reference.NoteCleanWord(i);
            fast.NoteCleanWord(i);
        }
        const Type type = types[rng() % types.size()];
        const bool clear = (rng() & 1) != 0;
        const u64 offset = rng() % (4 * Word);
        const u64 size = rng() % (4 * Word - offset + 1);
        std::vector<Range> expected, actual;
        reference_tracker.notifications.clear();
        fast_tracker.notifications.clear();
        reference.ForEachModifiedRange(type, clear, Base + offset, size,
            [&](u64 address, u64 length) { expected.emplace_back(address, length); }, false);
        fast.ForEachModifiedRange(type, clear, Base + offset, size,
            [&](u64 address, u64 length) { actual.emplace_back(address, length); }, true);
        REQUIRE(actual == expected);
        REQUIRE(fast.heap == reference.heap);
        REQUIRE(fast_tracker.notifications == reference_tracker.notifications);
        REQUIRE(fast.clean_words == reference.clean_words);
        REQUIRE(fast.CleanWordsHold());
    }
    // No CPU bits to upload, but the untracked page still needs a notification.
    fast.heap.fill(0);
    fast.Span(Type::Untracked)[1] = 1;
    for (size_t i = 0; i < fast.num_words; ++i) {
        fast.NoteCleanWord(i);
    }
    fast_tracker.notifications.clear();
    fast.ForEachModifiedRange(Type::CPU, true, Base, 4 * Word,
                              [](u64, u64) { FAIL("No CPU data should be uploaded"); }, true);
    REQUIRE(fast_tracker.notifications ==
            std::vector<std::tuple<VAddr, size_t, int>>{{Base + Word, Page, 1}});
    REQUIRE(fast.CleanWordsHold());
}

TEST_CASE("Last layout interval respects boundaries snapshot versions and address spaces",
          "[video_core][dkb_draw_experiments][dkb_layout_interval]") {
    using Tegra::PTEKind;
    Tegra::MemoryLayoutCache cache;
    std::vector<std::pair<s64, PTEKind>> kinds{
        {(std::numeric_limits<s64>::min)(), PTEKind::INVALID},
        {0x1000, PTEKind::PITCH}, {0x4000, PTEKind::INVALID},
        {0x8000, PTEKind::GENERIC_16BX2},
    };
    const auto reference = [&](GPUVAddr raw) -> size_t {
        const s64 address = static_cast<s64>(raw);
        if (address < 0) {
            return 0;
        }
        for (size_t i = kinds.size(); i-- > 0;) {
            if (kinds[i].first <= address) {
                if (kinds[i].second == PTEKind::INVALID) {
                    return 0;
                }
                const s64 end = i + 1 == kinds.size() ? (std::numeric_limits<s64>::max)()
                                                     : kinds[i + 1].first;
                return static_cast<size_t>(end - address);
            }
        }
        return 0;
    };
    std::mt19937_64 rng{0xd713f4462};
    for (u64 serial = 1; serial < 100; ++serial) {
        kinds[2].first = 0x2000 + static_cast<s64>(rng() % 0x2000);
        for (size_t owner : {size_t{1}, size_t{2}}) {
            // Another address space at the same serial must not reuse the previous boundary.
            kinds[2].first += 16;
            for (u64 i = 0; i < 100; ++i) {
                const GPUVAddr address = rng() % 0x10000;
                REQUIRE(cache.Query(owner, serial, kinds, address) == reference(address));
                REQUIRE(cache.Query(owner, serial, kinds, address + 1) == reference(address + 1));
            }
            for (const GPUVAddr address : {GPUVAddr{0}, GPUVAddr{0x1000},
                     static_cast<GPUVAddr>(kinds[2].first), GPUVAddr{0x8000},
                     static_cast<GPUVAddr>((std::numeric_limits<s64>::max)()), ~GPUVAddr{0}}) {
                REQUIRE(cache.Query(owner, serial, kinds, address) == reference(address));
            }
        }
    }
    REQUIRE(cache.Query(1, 100, kinds, 0x1000) != 0);
    kinds.resize(1); // Unmap the complete snapshot, including the cached interval.
    REQUIRE(cache.Query(1, 101, kinds, 0x1000) == 0);
}

TEST_CASE("Partial trap arming notifies only pages consumed or newly covered",
          "[video_core][dkb_draw_experiments][dkb_partial_read_traps][read_traps]") {
    VideoCommon::ReadTrapTracker traps;
    std::vector<Range> parts;
    const auto collect = [&](u64 address, u64 size) { parts.emplace_back(address, size); };
    REQUIRE(traps.Arm(Base, Page * 4, collect));
    REQUIRE(parts == std::vector<Range>{{Base, Page * 4}});
    traps.Subtract(Base + Page, Page);
    traps.Subtract(Base + Page * 3, Page);
    parts.clear();
    REQUIRE(traps.Arm(Base, Page * 4, collect));
    REQUIRE(parts == std::vector<Range>{{Base + Page, Page}, {Base + Page * 3, Page}});
    traps.Subtract(Base, Page);
    parts.clear();
    REQUIRE(traps.Arm(Base, Page * 6, collect));
    REQUIRE(parts == std::vector<Range>{{Base, Page}, {Base + Page * 4, Page * 2}});
    parts.clear();
    REQUIRE_FALSE(traps.Arm(Base, Page * 6, collect));
    REQUIRE(parts.empty());
    // Texture GetFlushArea can consume a wider area than the original CPU read.
    traps.Subtract(Base, Page * 6);
    REQUIRE(traps.Arm(Base, Page * 6, collect));
    REQUIRE(parts == std::vector<Range>{{Base, Page * 6}});
}

TEST_CASE("Partial read traps match a page oracle under overlapping writes and consumption",
          "[video_core][dkb_draw_experiments][dkb_partial_read_traps][read_traps]") {
    VideoCommon::ReadTrapTracker traps;
    std::array<bool, 128> armed{};
    std::mt19937 rng{264};
    for (size_t step = 0; step < 2000; ++step) {
        const size_t first = rng() % armed.size();
        const size_t count = rng() % (armed.size() - first + 1);
        if (rng() % 3 == 0) {
            traps.Subtract(Base + first * Page, count * Page);
            std::fill_n(armed.begin() + first, count, false);
            continue;
        }
        std::array<bool, 128> notified{};
        const bool any = traps.Arm(Base + first * Page, count * Page, [&](DAddr address, u64 size) {
            REQUIRE(address % Page == 0);
            REQUIRE(size % Page == 0);
            for (u64 page = (address - Base) / Page; page < (address - Base + size) / Page; ++page) {
                REQUIRE(page >= first);
                REQUIRE(page < first + count);
                REQUIRE_FALSE(notified[page]);
                notified[page] = true;
            }
        });
        bool expected_any = false;
        for (size_t i = first; i < first + count; ++i) {
            REQUIRE(notified[i] == !armed[i]);
            expected_any |= !armed[i];
            armed[i] = true;
        }
        REQUIRE(any == expected_any);
    }
}
