// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/settings_input.h"

namespace Core {
class System;
}
namespace InputCommon {
class InputSubsystem;
}

class AutoController {
public:
    AutoController();
    void Update(InputCommon::InputSubsystem& input, Core::System& system);

private:
    Settings::PlayerInput configured;
    Settings::PlayerInput keyboard;
};
