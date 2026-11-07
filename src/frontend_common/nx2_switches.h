// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

namespace FrontendCommon {

// Load executable defaults, then user overrides. Existing environment variables win.
void LoadNx2Switches(const char* argv0, const std::filesystem::path& config_dir);

} // namespace FrontendCommon
