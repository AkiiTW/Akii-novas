#pragma GCC diagnostic ignored "-Wmissing-declarations"
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <array>
#include <SDL3/SDL.h>
#include <SDL3/SDL_timer.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/perf_stats.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/keyboard.h"
#include "input_common/drivers/mouse.h"
#include "input_common/drivers/touch_screen.h"
#include "input_common/main.h"
#include "video_core/present_stats.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3.h"
#include "yuzu_cmd/yuzu_icon.h"

EmuWindow_SDL3::EmuWindow_SDL3(InputCommon::InputSubsystem* input_subsystem_, Core::System& system_)
    : input_subsystem{input_subsystem_}, system{system_} {
    input_subsystem->Initialize();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        LOG_CRITICAL(Frontend, "Failed to initialize SDL3: {}, Exiting...", SDL_GetError());
        exit(1);
    }
    next_title_update = SDL_GetTicks() + 2000;
    mouse_timer = SDL_AddTimer(100, [](void *userdata, SDL_TimerID, Uint32) -> Uint32 {
        auto* this_ = (EmuWindow_SDL3*)userdata;
        // The timer can fire before the input subsystem has created its mouse driver.
        if (auto* const mouse = this_->input_subsystem->GetMouse()) {
            mouse->NotifyChanged();
        }
        return 100;
    }, this);
}

EmuWindow_SDL3::~EmuWindow_SDL3() {
    SDL_RemoveTimer(mouse_timer);
    system.HIDCore().UnloadInputDevices();
    input_subsystem->Shutdown();
    SDL_Quit();
}

InputCommon::MouseButton EmuWindow_SDL3::SDLButtonToMouseButton(u32 button) const {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return InputCommon::MouseButton::Left;
    case SDL_BUTTON_RIGHT:
        return InputCommon::MouseButton::Right;
    case SDL_BUTTON_MIDDLE:
        return InputCommon::MouseButton::Wheel;
    case SDL_BUTTON_X1:
        return InputCommon::MouseButton::Backward;
    case SDL_BUTTON_X2:
        return InputCommon::MouseButton::Forward;
    default:
        return InputCommon::MouseButton::Undefined;
    }
}

/// @brief Translates pixel position to float position
EmuWindow_SDL3::FloatPairNonHFA EmuWindow_SDL3::MouseToTouchPos(s32 touch_x, s32 touch_y) const {
    int w = 0, h = 0;
    SDL_GetWindowSize(render_window, &w, &h);
    const float fx = float(touch_x) / w;
    const float fy = float(touch_y) / h;
    return {
        std::clamp<float>(fx, 0.0f, 1.0f),
        std::clamp<float>(fy, 0.0f, 1.0f),
        0
    };
}

void EmuWindow_SDL3::OnMouseButton(u32 button, u8 state, s32 x, s32 y) {
    const auto mouse_button = SDLButtonToMouseButton(button);
    if (state != 0) {
        auto const [touch_x, touch_y, _] = MouseToTouchPos(x, y);
        input_subsystem->GetMouse()->PressButton(x, y, mouse_button);
        input_subsystem->GetMouse()->PressMouseButton(mouse_button);
        input_subsystem->GetMouse()->PressTouchButton(touch_x, touch_y, mouse_button);
    } else {
        input_subsystem->GetMouse()->ReleaseButton(mouse_button);
    }
    input_subsystem->GetMouse()->NotifyChanged();
}

void EmuWindow_SDL3::OnMouseMotion(s32 x, s32 y) {
    auto const [touch_x, touch_y, _] = MouseToTouchPos(x, y);
    input_subsystem->GetMouse()->Move(x, y, 0, 0);
    input_subsystem->GetMouse()->MouseMove(touch_x, touch_y);
    input_subsystem->GetMouse()->TouchMove(touch_x, touch_y);
    input_subsystem->GetMouse()->NotifyChanged();
}

void EmuWindow_SDL3::OnFingerDown(float x, float y, std::size_t id) {
    input_subsystem->GetTouchScreen()->TouchPressed(x, y, id);
}

void EmuWindow_SDL3::OnFingerMotion(float x, float y, std::size_t id) {
    input_subsystem->GetTouchScreen()->TouchMoved(x, y, id);
}

void EmuWindow_SDL3::OnFingerUp() {
    input_subsystem->GetTouchScreen()->ReleaseAllTouch();
}

// Seconds since the first frame; shared by the key script (yuzu.cpp) and key recording.
double Nx2KeyClockSeconds() {
    static const auto begin = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
}

void EmuWindow_SDL3::OnKeyEvent(int key, u8 state) {
    // EDEN_NX2_KEY_RECORD=<file>: append each press/release ("seconds:code", "seconds:-code").
    static std::FILE* const record = [] {
        const char* path = std::getenv("EDEN_NX2_KEY_RECORD");
        return path ? std::fopen(path, "w") : nullptr;
    }();
    static std::array<bool, 512> held{};
    if (record && key >= 0 && key < static_cast<int>(held.size()) && held[key] != (state != 0)) {
        held[key] = state != 0;
        std::fprintf(record, "f%llu:%s%d,",
                     static_cast<unsigned long long>(Core::g_nx2_present_count.load()),
                     state != 0 ? "" : "-", key);
        std::fflush(record);
    }
    if (state != 0) {
        input_subsystem->GetKeyboard()->PressKey(static_cast<std::size_t>(key));
    } else {
        input_subsystem->GetKeyboard()->ReleaseKey(static_cast<std::size_t>(key));
    }
}

bool EmuWindow_SDL3::IsOpen() const {
    return is_open;
}

void EmuWindow_SDL3::ApplyPendingTitle() {
    const auto now = SDL_GetTicks();
    if (now < next_title_update) {
        return;
    }
    next_title_update = now + 2000;
    // Reading stats on this thread avoids racing the core's shutdown.
    const auto results = system.GetAndResetPerfStats();
    static const bool log_fps = std::getenv("EDEN_NX2_FPS_LOG") != nullptr;
    if (log_fps) {
        LOG_INFO(Frontend, "FPS: {:.1f} speed: {:.0f}% frametime: {:.2f} ms", results.average_game_fps,
                 results.emulation_speed * 100.0, results.frametime * 1000.0);
    }
    // With frame generation the screen shows more frames than the game draws: show both.
    static u64 last_presented = VideoCore::PresentedFrameCount();
    static Uint64 last_presented_ticks = now;
    const u64 presented = VideoCore::PresentedFrameCount();
    const double seconds = static_cast<double>(now - last_presented_ticks) / 1000.0;
    const double shown_fps = seconds > 0.0 ? static_cast<double>(presented - last_presented) / seconds : 0.0;
    last_presented = presented;
    last_presented_ticks = now;
    const bool generating = Settings::values.frame_gen.GetValue() &&
                            shown_fps > results.average_game_fps * 1.2 + 1.0;
    if (log_fps && generating) {
        LOG_INFO(Frontend, "Shown FPS with frame generation: {:.1f}", shown_fps);
    }
    const auto title = generating ? fmt::format("{:.0f} FPS en pantalla ({:.0f} del juego + FSR3)",
                                                shown_fps, results.average_game_fps)
                                  : fmt::format("{:.0f} FPS", results.average_game_fps);
    const auto caption = game_title.empty()
                             ? fmt::format("NovaS \xC2\xB7 {}", title)
                             : fmt::format("NovaS \xC2\xB7 {} \xC2\xB7 {}", game_title, title);
    SDL_SetWindowTitle(render_window, caption.c_str());
}

void EmuWindow_SDL3::SetCaption(const std::string& caption) {
    SDL_SetWindowTitle(render_window, caption.c_str());
}

bool EmuWindow_SDL3::IsShown() const {
    return is_shown;
}

void EmuWindow_SDL3::OnResize() {
    int width, height;
    SDL_GetWindowSizeInPixels(render_window, &width, &height);
    UpdateCurrentFramebufferLayout(width, height);
}

void EmuWindow_SDL3::ShowCursor(bool show_cursor) {
    if (show_cursor) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
}

void EmuWindow_SDL3::Fullscreen() {
    fullscreen_fallback = false;
    SDL_DisplayMode display_mode;
    switch (Settings::values.fullscreen_mode.GetValue()) {
    case Settings::FullscreenMode::Exclusive:
        // SDL restores the windowed size when leaving fullscreen.
        if (const SDL_DisplayMode* display_mode_ptr = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(render_window))) {
            display_mode = *display_mode_ptr;
            SDL_SetWindowFullscreenMode(render_window, &display_mode);
        } else {
            LOG_ERROR(Frontend, "SDL_GetDesktopDisplayMode failed: {}", SDL_GetError());
        }

        if (SDL_SetWindowFullscreen(render_window, true)) {
            return;
        }

        LOG_ERROR(Frontend, "Fullscreening failed: {}", SDL_GetError());
        LOG_INFO(Frontend, "Attempting to use borderless fullscreen...");
        [[fallthrough]];
    case Settings::FullscreenMode::Borderless:
        SDL_SetWindowFullscreenMode(render_window, nullptr);
        if (SDL_SetWindowFullscreen(render_window, true)) {
            return;
        }

        LOG_ERROR(Frontend, "Borderless fullscreening failed: {}", SDL_GetError());
        [[fallthrough]];
    default:
        // Fallback algorithm: Maximise window.
        // Works on all systems (unless something is seriously wrong), so no fallback for this one.
        LOG_INFO(Frontend, "Falling back on a maximised window...");
        fullscreen_fallback = SDL_MaximizeWindow(render_window);
        break;
    }
}

void EmuWindow_SDL3::ToggleFullscreen() {
    const auto flags = SDL_GetWindowFlags(render_window);
    if (flags & SDL_WINDOW_FULLSCREEN) {
        ignore_maximize_until_ns = SDL_GetTicksNS() + 1'000'000'000ULL;
        if (!SDL_SetWindowFullscreen(render_window, false)) {
            LOG_ERROR(Frontend, "Could not leave fullscreen: {}", SDL_GetError());
            return;
        }
        // A window maximized before going fullscreen would come back maximized: back to a normal
        // window, so the bar and its buttons are there and maximize can be pressed again.
        if (SDL_GetWindowFlags(render_window) & SDL_WINDOW_MAXIMIZED) {
            SDL_RestoreWindow(render_window);
        }
        ShowCursor(true);
    } else if (fullscreen_fallback && (flags & SDL_WINDOW_MAXIMIZED)) {
        SDL_RestoreWindow(render_window);
        fullscreen_fallback = false;
        ShowCursor(true);
    } else {
        Fullscreen();
        ShowCursor(false);
    }
}

void EmuWindow_SDL3::UpdateExitShortcut() {
    if (!(SDL_GetWindowFlags(render_window) & SDL_WINDOW_INPUT_FOCUS)) {
        exit_chords.clear();
        return;
    }
    const auto now = SDL_GetTicksNS();
    for (const auto& [id, chord] : exit_chords) {
        if (chord.held_since && now - *chord.held_since >= 1'500'000'000ULL) {
            LOG_INFO(Frontend, "NovaS exit requested: Back + Start held on controller {}", id);
            is_open = false;
            return;
        }
    }
}

void EmuWindow_SDL3::OnEvent(SDL_Event& event) {
    // Notice how we skip the "update title" aspect on most events
    // this is because some WMs do NOT like changing titles while resizing
    // so let's just... not do that, thanks :)
    // Afterall we don't really expect the user to pay attention to the titlebar
    // while they're moving a lot of shit around...
    switch (event.type) {
    case SDL_EVENT_WINDOW_MAXIMIZED:
        // The maximize button goes fullscreen (no window bar, no taskbar), like the start does.
        if (!fullscreen_fallback && SDL_GetTicksNS() >= ignore_maximize_until_ns &&
            !(SDL_GetWindowFlags(render_window) & SDL_WINDOW_FULLSCREEN)) {
            LOG_INFO(Frontend, "NovaS: maximize goes fullscreen");
            SDL_RestoreWindow(render_window);
            Fullscreen();
            ShowCursor(false);
        }
        return OnResize();
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESTORED:
        return OnResize();
    case SDL_EVENT_WINDOW_MINIMIZED:
        is_shown = false;
        return OnResize();
    case SDL_EVENT_WINDOW_EXPOSED:
        is_shown = true;
        return OnResize();
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        LOG_INFO(Frontend, "Window close requested");
        is_open = false;
        return;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        // Escape leaves fullscreen so the window bar (and its close button) shows; it never quits.
        if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
            if (event.key.down && !event.key.repeat) {
                const auto flags = SDL_GetWindowFlags(render_window);
                if ((flags & SDL_WINDOW_FULLSCREEN) ||
                    (fullscreen_fallback && (flags & SDL_WINDOW_MAXIMIZED))) {
                    LOG_INFO(Frontend, "NovaS: Escape leaves fullscreen");
                    ToggleFullscreen();
                }
            }
            return;
        }
        if (event.key.scancode == SDL_SCANCODE_F11) {
            if (event.key.down && !event.key.repeat) {
                ToggleFullscreen();
            }
            return;
        }
        if (event.key.scancode == SDL_SCANCODE_RETURN) {
            if (fullscreen_enter_held) {
                if (!event.key.down) {
                    fullscreen_enter_held = false;
                }
                return;
            }
            if (event.key.down && !event.key.repeat && (event.key.mod & SDL_KMOD_ALT)) {
                fullscreen_enter_held = true;
                ToggleFullscreen();
                return;
            }
        }
        return OnKeyEvent(int(event.key.scancode), event.key.down ? 1 : 0);
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        input_subsystem->GetKeyboard()->ReleaseAllKeys();
        fullscreen_enter_held = false;
        exit_chords.clear();
        return;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        if (!(SDL_GetWindowFlags(render_window) & SDL_WINDOW_INPUT_FOCUS) ||
            (event.gbutton.button != SDL_GAMEPAD_BUTTON_BACK &&
             event.gbutton.button != SDL_GAMEPAD_BUTTON_START)) {
            return;
        }
        auto& chord = exit_chords[event.gbutton.which];
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
            chord.back = event.gbutton.down;
        } else {
            chord.start = event.gbutton.down;
        }
        if (chord.back && chord.start) {
            if (!chord.held_since) {
                chord.held_since = SDL_GetTicksNS();
            }
        } else {
            chord.held_since.reset();
        }
        // The input driver's event watcher still delivers normal Minus/Plus presses to the game.
        return;
    }
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
        exit_chords.erase(event.gdevice.which);
        return;
    case SDL_EVENT_MOUSE_MOTION:
        // ignore if it came from touch
        if (event.button.which != SDL_TOUCH_MOUSEID)
            OnMouseMotion(event.motion.x, event.motion.y);
        return;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        // ignore if it came from touch
        if (event.button.which != SDL_TOUCH_MOUSEID)
            OnMouseButton(event.button.button, event.button.down ? 1 : 0, s32(event.button.x), s32(event.button.y));
        return;
    case SDL_EVENT_FINGER_DOWN:
        return OnFingerDown(event.tfinger.x, event.tfinger.y, std::size_t(event.tfinger.touchID));
    case SDL_EVENT_FINGER_MOTION:
        return OnFingerMotion(event.tfinger.x, event.tfinger.y, std::size_t(event.tfinger.touchID));
    case SDL_EVENT_FINGER_UP:
        return OnFingerUp();
    case SDL_EVENT_QUIT:
        LOG_INFO(Frontend, "Quit requested");
        is_open = false;
        return;
    default:
        break;
    }
}

// Credits to Samantas5855 and others for this function.
void EmuWindow_SDL3::SetWindowIcon() {
#if defined(__EMSCRIPTEN__) || defined(__wasi__)
    // Icons do not work yet
#else
    SDL_IOStream* const yuzu_icon_stream = SDL_IOFromConstMem((void*)yuzu_icon, yuzu_icon_size);
    if (yuzu_icon_stream == nullptr) {
        LOG_WARNING(Frontend, "Failed to create NovaS icon stream.");
        return;
    }
    SDL_Surface* const window_icon = SDL_LoadBMP_IO(yuzu_icon_stream, true);
    if (window_icon == nullptr) {
        LOG_WARNING(Frontend, "Failed to read BMP from stream.");
        return;
    }
    // The icon is attached to the window pointer
    SDL_SetWindowIcon(render_window, window_icon);
    SDL_DestroySurface(window_icon);
#endif
}

void EmuWindow_SDL3::OnMinimalClientAreaChangeRequest(std::pair<u32, u32> minimal_size) {
    SDL_SetWindowMinimumSize(render_window, minimal_size.first, minimal_size.second);
}
