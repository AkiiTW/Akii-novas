// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/fragment_color_mask.h"

TEST_CASE("Only present fragment shaders without color exports suppress masks",
          "[fragment_color_mask]") {
    std::array<bool, 8> outputs{};
    CHECK(Vulkan::FragmentHasNoColorOutputs(true, outputs));
    CHECK_FALSE(Vulkan::FragmentHasNoColorOutputs(false, outputs));
    for (size_t index = 0; index < outputs.size(); ++index) {
        outputs.fill(false);
        outputs[index] = true;
        CAPTURE(index);
        CHECK_FALSE(Vulkan::FragmentHasNoColorOutputs(true, outputs));
    }
    // Includes the two outputs that may be mapped to a dual-source attachment.
    outputs.fill(false);
    outputs[0] = outputs[1] = true;
    CHECK_FALSE(Vulkan::FragmentHasNoColorOutputs(true, outputs));
}

TEST_CASE("Static and dynamic fragment masks agree for every guest component mask",
          "[fragment_color_mask]") {
    Vulkan::FragmentColorMaskState state;
    Tegra::Engines::Maxwell3D::DirtyState::Flags flags{};
    for (const bool no_outputs : {false, true, false}) {
        state.Update(no_outputs, flags);
        for (u32 mask = 0; mask < 16; ++mask) {
            CAPTURE(no_outputs, mask);
            const u32 expected = no_outputs ? 0 : mask;
            CHECK(Vulkan::FragmentColorWriteMask(mask, no_outputs) == expected);
            CHECK(state.Apply(mask) == expected);
        }
    }
}

TEST_CASE("Fragment export changes invalidate both blending and color masks",
          "[fragment_color_mask]") {
    Vulkan::FragmentColorMaskState state;
    Tegra::Engines::Maxwell3D::DirtyState::Flags flags{};
    state.Update(false, flags);
    CHECK(flags.none());

    // The guest masks stay fixed while pipelines alternate between no exports and exports.
    constexpr std::array<u32, 8> guest_masks{15, 1, 2, 4, 8, 0, 5, 10};
    for (const bool no_outputs : {true, false, true}) {
        flags.reset();
        flags[Vulkan::Dirty::Scissors] = true;
        state.Update(no_outputs, flags);
        CHECK(flags[Vulkan::Dirty::Blending]);
        CHECK(flags[Vulkan::Dirty::ColorMask]);
        CHECK(flags[Vulkan::Dirty::Scissors]);
        CHECK(flags.count() == 3);
        for (const u32 mask : guest_masks) {
            CHECK(state.Apply(mask) == (no_outputs ? 0 : mask));
        }

        // After those flags have been consumed, rebinding the same state is a no-op.
        flags.reset();
        state.Update(no_outputs, flags);
        CHECK(flags.none());
    }

    flags[Vulkan::Dirty::Blending] = true;
    flags[Vulkan::Dirty::ColorMask] = true;
    state.Update(true, flags);
    CHECK(flags[Vulkan::Dirty::Blending]);
    CHECK(flags[Vulkan::Dirty::ColorMask]);
}
