// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <utility>
#include <vector>

#include "common/logging.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/core.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/main.h"
#include "yuzu_cmd/auto_controller.h"
#include "yuzu_cmd/sdl_config.h"

namespace {
bool HasConnectedController(const Settings::PlayerInput& player,
                            const std::vector<Common::ParamPackage>& devices) {
    if (!player.connected) {
        return false;
    }
    bool found_controller = false;
    const auto connected = [&](const std::string& binding) {
        const Common::ParamPackage params{binding};
        const auto engine = params.Get("engine", "");
        if (engine.empty() || engine == "keyboard" || engine == "analog_from_button" ||
            engine == "mouse") {
            return true;
        }
        found_controller = true;
        return std::ranges::any_of(devices, [&](const auto& device) {
            if (device.Get("engine", "") != engine) {
                return false;
            }
            for (const auto* key : {"guid", "port", "pad"}) {
                if (params.Has(key) && params.Get(key, "") != device.Get(key, "")) {
                    return false;
                }
            }
            return true;
        });
    };
    const bool available = std::ranges::all_of(player.buttons, connected) &&
                           std::ranges::all_of(player.analogs, connected);
    return found_controller && available;
}

bool SameBindings(const Settings::PlayerInput& lhs, const Settings::PlayerInput& rhs) {
    return lhs.connected == rhs.connected && lhs.controller_type == rhs.controller_type &&
           lhs.buttons == rhs.buttons && lhs.analogs == rhs.analogs && lhs.motions == rhs.motions;
}
} // namespace

AutoController::AutoController()
    : configured{Settings::values.players.GetValue()[0]}, keyboard{configured} {
    keyboard.connected = true;
    keyboard.controller_type = Settings::ControllerType::ProController;
    for (std::size_t i = 0; i < keyboard.buttons.size(); ++i) {
        if (Common::ParamPackage{keyboard.buttons[i]}.Get("engine", "") != "keyboard") {
            keyboard.buttons[i] = InputCommon::GenerateKeyboardParam(SdlConfig::default_buttons[i]);
        }
    }
    for (std::size_t i = 0; i < keyboard.analogs.size(); ++i) {
        if (Common::ParamPackage{keyboard.analogs[i]}.Get("engine", "") != "analog_from_button") {
            const auto& keys = SdlConfig::default_analogs[i];
            keyboard.analogs[i] = InputCommon::GenerateAnalogParamFromKeys(
                keys[0], keys[1], keys[2], keys[3], SdlConfig::default_stick_mod[i], 0.5f);
        }
    }
    for (std::size_t i = 0; i < keyboard.motions.size(); ++i) {
        if (Common::ParamPackage{keyboard.motions[i]}.Get("engine", "") != "keyboard") {
            keyboard.motions[i] = InputCommon::GenerateKeyboardParam(SdlConfig::default_motions[i]);
        }
    }
}

void AutoController::Update(InputCommon::InputSubsystem& input, Core::System& system) {
    const auto devices = input.GetInputDevices();
    auto& player = Settings::values.players.GetValue()[0];
    if (HasConnectedController(player, devices)) {
        return;
    }

    auto replacement = keyboard;
    std::string device_name = "keyboard";
    if (HasConnectedController(configured, devices)) {
        replacement = configured;
        device_name = "configured controller";
    } else {
        for (const auto& device : devices) {
            if (device.Get("engine", "") != "sdl") {
                continue;
            }
            const auto buttons = input.GetButtonMappingForDevice(device);
            const auto analogs = input.GetAnalogMappingForDevice(device);
            if (buttons.empty() || analogs.empty()) {
                continue;
            }
            const auto motions = input.GetMotionMappingForDevice(device);
            for (const auto& [index, params] : buttons) {
                replacement.buttons[index] = params.Serialize();
            }
            for (const auto& [index, params] : analogs) {
                replacement.analogs[index] = params.Serialize();
            }
            for (const auto& [index, params] : motions) {
                replacement.motions[index] = params.Serialize();
            }
            device_name = device.Get("display", "SDL controller");
            break;
        }
    }

    if (SameBindings(player, replacement)) {
        return;
    }
    // Session-only bindings: the saved configuration remains the user's preference.
    player = std::move(replacement);
    system.HIDCore().GetEmulatedControllerByIndex(0)->ReloadFromSettings();
    LOG_INFO(Input, "NovaS player 1: {}", device_name);
}
