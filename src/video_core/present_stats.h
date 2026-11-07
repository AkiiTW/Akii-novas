// SPDX-FileCopyrightText: Copyright 2026 NovaS
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace VideoCore {

/// Images handed to the swapchain so far, generated frames (frame generation) included.
/// The game's own FPS counts only the frames the game drew.
u64 PresentedFrameCount();

} // namespace VideoCore
