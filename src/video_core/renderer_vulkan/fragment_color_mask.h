// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"

namespace Vulkan {

inline bool FragmentHasNoColorOutputs(bool fragment_present, std::span<const bool> color_outputs) {
    return fragment_present &&
           std::ranges::none_of(color_outputs, [](bool written) { return written; });
}

inline u32 FragmentColorWriteMask(u32 guest_mask, bool no_color_outputs) {
    // Depth-only fragment programs must preserve any bound color attachments.
    return no_color_outputs ? 0 : guest_mask;
}

class FragmentColorMaskState {
public:
    void Update(bool no_color_outputs, Tegra::Engines::Maxwell3D::DirtyState::Flags& flags) {
        if (no_color_outputs == last_no_color_outputs) {
            return;
        }
        last_no_color_outputs = no_color_outputs;
        // A pipeline change can require new dynamic masks without a guest register write.
        flags[Dirty::Blending] = true;
        flags[Dirty::ColorMask] = true;
    }

    u32 Apply(u32 guest_mask) const {
        return FragmentColorWriteMask(guest_mask, last_no_color_outputs);
    }

private:
    bool last_no_color_outputs{};
};

} // namespace Vulkan
