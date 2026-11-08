// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <iterator>
#include <limits>
#include <span>
#include <utility>

#include "common/common_types.h"
#include "video_core/pte_kind.h"

namespace Tegra {

// A caller-owned cache for immutable snapshots. Use a thread-local instance: readers of one
// MemoryManager may be on different threads, and its object address may later be reused.
class MemoryLayoutCache {
public:
    size_t Query(size_t owner, u64 serial, std::span<const std::pair<s64, PTEKind>> kinds,
                 GPUVAddr gpu_addr) {
        const s64 address = static_cast<s64>(gpu_addr);
        if (address < 0) {
            return 0;
        }
        if (valid && owner == cached_owner && serial == cached_serial &&
            address >= cached_begin && address < cached_end) {
            return static_cast<size_t>(cached_end - address);
        }
        const auto it = std::upper_bound(kinds.begin(), kinds.end(), address,
                                        [](s64 key, const auto& range) { return key < range.first; });
        if (it == kinds.begin() || std::prev(it)->second == PTEKind::INVALID) {
            return 0;
        }
        cached_owner = owner;
        cached_serial = serial;
        cached_begin = std::prev(it)->first;
        cached_end = it == kinds.end() ? (std::numeric_limits<s64>::max)() : it->first;
        valid = true;
        return static_cast<size_t>(cached_end - address);
    }

private:
    size_t cached_owner{};
    u64 cached_serial{};
    s64 cached_begin{};
    s64 cached_end{};
    bool valid{};
};

} // namespace Tegra
