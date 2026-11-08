// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

namespace VideoCommon::Nx2DrawExperiments {

inline bool ReadSwitch(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

// Function-local initialization: nx2_switches.txt is applied before these GPU paths run.
inline bool CleanWords() {
    static const bool enabled = ReadSwitch("NOVAS_DKB_CLEAN_WORDS");
    return enabled;
}

inline bool LayoutInterval() {
    static const bool enabled = ReadSwitch("NOVAS_DKB_LAYOUT_INTERVAL");
    return enabled;
}

inline bool PartialReadTraps() {
    static const bool enabled = ReadSwitch("NOVAS_DKB_PARTIAL_READ_TRAPS");
    return enabled;
}

inline bool VertexStores() {
    static const bool enabled = ReadSwitch("NOVAS_DKB_VERTEX_STORES");
    return enabled;
}

} // namespace VideoCommon::Nx2DrawExperiments
