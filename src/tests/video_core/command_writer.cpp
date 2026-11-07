// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <limits>
#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "common/scope_exit.h"
#include "core/device_memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/dma_pusher.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/macro.h"
#include "video_core/memory_manager.h"
#include "video_core/gpu.h"
#include "video_core/nx2.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace Tegra {

struct DmaPusherTestAccess {
    static bool ListPagesCandidate(DmaPusher& pusher, GPUVAddr address, u64 size) {
        return pusher.SkipWriterWaitPages(address, size);
    }
    static bool IsMemoryWriter(const Engines::EngineInterface& engine, Engines::EngineTypes type,
                               u32 method) {
        return DmaPusher::IsMemoryWriter(engine, type, method);
    }
};

} // namespace Tegra

TEST_CASE("List page candidates reject aliases of written pages, command pages and holes",
          "[video_core][list_pages]") {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    memory.BindRasterizer(&rasterizer);
    Tegra::Control::ChannelState channel{0};
    Tegra::DmaPusher pusher{system, memory, channel};
    const bool was_ampere = Tegra::g_nx2_ampere.exchange(true);
    Tegra::Nx2ClearGpuWrittenPages();
    SCOPE_EXIT {
        Tegra::Nx2ClearGpuWrittenPages();
        Tegra::g_nx2_ampere.store(was_ampere);
    };
    constexpr GPUVAddr address = 0x100000;
    constexpr DAddr device = 0x10000;
    memory.Map(address, device, 0x2000, Tegra::PTEKind::PITCH, false);
    memory.Map(address + 0x10000, device, 0x1000, Tegra::PTEKind::PITCH, false);
    using Access = Tegra::DmaPusherTestAccess;
    REQUIRE(Access::ListPagesCandidate(pusher, address + 0xffc, 8));
    REQUIRE_FALSE(Access::ListPagesCandidate(pusher, address + 0x1ffc, 8));
    REQUIRE_FALSE(Access::ListPagesCandidate(pusher, ~u64{0} - 3, 16));
    REQUIRE_FALSE(Access::ListPagesCandidate(pusher, address, 0));
    Tegra::Nx2MarkGpuWritten(device, 4);
    REQUIRE_FALSE(Access::ListPagesCandidate(pusher, address + 0x10000, 4));
    Tegra::Nx2ClearGpuWrittenPages();
    Tegra::Nx2MarkCommandListPage(device + 0x1000);
    REQUIRE_FALSE(Access::ListPagesCandidate(pusher, address + 0xffc, 8));
    REQUIRE(Access::ListPagesCandidate(pusher, address, 4));
}

TEST_CASE("Command writer classification excludes only plain compute and DMA state",
          "[video_core][command_writer]") {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    Tegra::Engines::KeplerCompute compute{memory};
    Tegra::Engines::MaxwellDMA dma{memory};
    Tegra::MacroEngine macros{true};
    using Access = Tegra::DmaPusherTestAccess;
    using Type = Tegra::Engines::EngineTypes;
    for (const bool native : {false, true}) {
        CAPTURE(native);
        compute.SetNativeMacroMode(native ? &macros : nullptr, {});
        for (u32 method = 0; method < 0xe00; ++method) {
            CAPTURE(method);
            const bool writer = Access::IsMemoryWriter(compute, Type::KeplerCompute, method);
            if (!native) {
                REQUIRE(writer == compute.execution_mask[method]);
            } else {
                const bool traced = std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr &&
                                    (method == 0x5a || (method >= 0xc6 && method <= 0x108));
                const bool effect = traced || method == 0x45 || method == 0x46 || method == 0x47 ||
                                    method == 0x48 || method == 0xae || method == 0xaf ||
                                    method == 0xb0 || method == 0x107 || method == 0x6c3 ||
                                    method == KEPLER_COMPUTE_REG_INDEX(exec_upload) ||
                                    method == KEPLER_COMPUTE_REG_INDEX(data_upload);
                REQUIRE(writer == effect);
            }
        }
    }
    for (u32 method = 0; method < 0xe00; ++method) {
        CAPTURE(method);
        REQUIRE(Access::IsMemoryWriter(dma, Type::MaxwellDMA, method) ==
                dma.execution_mask[method]);
    }
    for (const u32 method : {0xe00U, 0xfffU, 0xffffU, std::numeric_limits<u32>::max()}) {
        CAPTURE(method);
        REQUIRE(Access::IsMemoryWriter(compute, Type::KeplerCompute, method));
        REQUIRE(Access::IsMemoryWriter(dma, Type::MaxwellDMA, method));
    }
}
