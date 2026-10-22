/*
 * An SDL gamepad as a DualShock 2 reads it (Display::PadState) - the Vulkan
 * display's and the host's plain SDL window's alike.
 */

#ifndef PS2GPU_SDL_PAD_H
#define PS2GPU_SDL_PAD_H

#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <cstdint>
#include <utility>

#include "ps2gpu/display.h"

namespace ps2gpu {

inline std::uint8_t SdlPadFamily(SDL_Gamepad* pad) {
    switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5:
            return Display::kPlayStationController;
        case SDL_GAMEPAD_TYPE_XBOX360:
        case SDL_GAMEPAD_TYPE_XBOXONE:
            return Display::kXboxController;
        default:
            return Display::kOtherController;
    }
}

/* One controller as a DualShock 2 reads. */
inline Display::PadState ReadSdlPad(SDL_Gamepad* pad) {
    // SDL's buttons by position (south is the DualShock's cross) to
    // DualShock 2 inputs.
    static const std::pair<SDL_GamepadButton, int> kButtons[] = {
        {SDL_GAMEPAD_BUTTON_BACK, 0}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, 1}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 2},
        {SDL_GAMEPAD_BUTTON_START, 3}, {SDL_GAMEPAD_BUTTON_DPAD_UP, 4}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 5},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 6}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 7},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 10}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 11},
        {SDL_GAMEPAD_BUTTON_NORTH, 12}, {SDL_GAMEPAD_BUTTON_EAST, 13}, {SDL_GAMEPAD_BUTTON_SOUTH, 14},
        {SDL_GAMEPAD_BUTTON_WEST, 15}};
    Display::PadState s;
    for (const auto& [button, input] : kButtons)
        if (SDL_GetGamepadButton(pad, button)) s.buttons |= std::uint16_t(1u << input);
    // Triggers (0-32767) are L2 and R2: pressed past a quarter of their travel.
    const int l2 = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    const int r2 = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    s.l2 = std::uint8_t(std::clamp(l2, 0, 32767) >> 7);
    s.r2 = std::uint8_t(std::clamp(r2, 0, 32767) >> 7);
    if (l2 > 8192) s.buttons |= 1u << 8;
    if (r2 > 8192) s.buttons |= 1u << 9;
    // Sticks: -32768-32767 to 0-255, down and right the larger, as the
    // DualShock's; a small dead zone reads as centred.
    static const SDL_GamepadAxis kAxes[4] = {SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
                                             SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY};
    for (int i = 0; i < 4; ++i) {
        const int v = SDL_GetGamepadAxis(pad, kAxes[i]);
        s.sticks[i] = (v > -4096 && v < 4096) ? 0x80 : std::uint8_t((v + 32768) >> 8);
    }
    s.family = SdlPadFamily(pad);
    return s;
}

}  // namespace ps2gpu

#endif  // PS2GPU_SDL_PAD_H
