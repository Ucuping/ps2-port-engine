/*
 * The plain SDL window's tests (host/src/platform/sdl_display.cpp), where it
 * is built - outside Windows, with SDL. On SDL's dummy video driver (a
 * window that is a surface in memory - the offscreen one asks macOS for EGL)
 * and its software renderer, so they need no screen: the picture's colours as
 * the GS gives them (BGRA words), its aspect in the window, the window's
 * close, and a gamepad - SDL's virtual one - read as a DualShock 2.
 */

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "platform/sdl_display.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

// The window's pixel at (x, y), as 0xRRGGBB, from what was presented last
// (the software renderer draws into the window's surface, which keeps it).
std::uint32_t Shown(int x, int y) {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_Renderer* renderer = count > 0 ? SDL_GetRenderer(windows[0]) : nullptr;
    SDL_free(windows);
    SDL_Surface* surface = renderer ? SDL_RenderReadPixels(renderer, nullptr) : nullptr;
    if (!surface) return 0xFFFFFFFFu;
    Uint8 r = 0, g = 0, b = 0, a = 0;
    const bool read = SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a);
    SDL_DestroySurface(surface);
    return read ? (std::uint32_t(r) << 16 | std::uint32_t(g) << 8 | b) : 0xFFFFFFFFu;
}

constexpr int kWidth = 640, kHeight = 360;

// A 64x48 picture (4:3) in quarters: red, green above; blue, white below.
// BGRA words as Gs::Picture gives them, with the GS's alpha (0x80) in the
// top byte, which the window ignores.
std::vector<std::uint32_t> Quarters() {
    std::vector<std::uint32_t> picture(64 * 48);
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 64; ++x) {
            const bool right = x >= 32, below = y >= 24;
            picture[y * 64 + x] = 0x80000000u | (below ? (right ? 0xFFFFFFu : 0x0000FFu)
                                                       : (right ? 0x00FF00u : 0xFF0000u));
        }
    return picture;
}

void ThePictureIsShownAtItsAspect(ps2gpu::Display& display) {
    const std::vector<std::uint32_t> picture = Quarters();
    Check(display.Present(picture.data(), 64, 48), "a picture is presented");
    // 4:3 in 640x360: 480x360, centred - black on either side.
    Check(Shown(40, 180) == 0x000000, "4:3 in a wide window: black to the left");
    Check(Shown(600, 180) == 0x000000, "4:3 in a wide window: black to the right");
    Check(Shown(200, 90) == 0xFF0000, "the picture's top left is red (its BGRA read as such)");
    Check(Shown(440, 90) == 0x00FF00, "the picture's top right is green");
    Check(Shown(200, 270) == 0x0000FF, "the picture's bottom left is blue");
    Check(Shown(440, 270) == 0xFFFFFF, "the picture's bottom right is white");
    // The window's own shape ("auto"): the whole window.
    display.SetAspect(0.0f);
    Check(display.Present(picture.data(), 64, 48), "a picture is presented at the window's shape");
    Check(Shown(40, 90) == 0xFF0000, "at the window's shape the picture reaches its left edge");
    Check(Shown(600, 270) == 0xFFFFFF, "at the window's shape the picture reaches its right edge");
    display.SetAspect(4.0f / 3.0f);
}

void AGamepadReadsAsADualShock(ps2gpu::Display& display) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Test gamepad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    Check(id != 0, "a virtual gamepad is plugged in");
    if (!id) return;
    SDL_Joystick* joystick = SDL_OpenJoystick(id);
    display.Pump();                     // its arrival: the window takes it
    const std::vector<ps2gpu::Display::ControllerInfo> list = display.Controllers();
    Check(list.size() == 1 && list[0].id == std::uint32_t(id), "the window lists the gamepad");
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_START, true);
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, -32768);
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTY, 32767);
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTX, 1000);    // in the dead zone
    display.Pump();                     // the state taken
    ps2gpu::Display::PadState pad;
    Check(display.Pad(&pad), "the window reads the gamepad");
    // Cross 14, Up 4, Start 3, and L2 8 from its trigger (libpad's order).
    Check(pad.buttons == ((1u << 14) | (1u << 4) | (1u << 3) | (1u << 8)), "the buttons as a DualShock 2's");
    Check(pad.l2 == 255 && pad.r2 == 0, "the triggers' travel");
    Check(pad.sticks[2] == 0x00, "the left stick held left reads 0");
    Check(pad.sticks[3] == 0x80, "the left stick's other axis is centred");
    Check(pad.sticks[1] == 0xFF, "the right stick held down reads 255");
    Check(pad.sticks[0] == 0x80, "a stick inside the dead zone reads centred");
    ps2gpu::Display::PadState by_id;
    Check(display.PadOf(std::uint32_t(id), &by_id) && by_id.buttons == pad.buttons, "the gamepad read by its id");
    SDL_CloseJoystick(joystick);
    SDL_DetachVirtualJoystick(id);
    display.Pump();                     // its leaving
    Check(display.Controllers().empty() && !display.Pad(&pad), "an unplugged gamepad is gone");
}

void ClosingTheWindowEndsIt(ps2gpu::Display& display) {
    Check(display.Pump(), "an open window pumps");
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
    Check(!display.Pump(), "the window closed: its pump says so");
    const std::vector<std::uint32_t> picture = Quarters();
    Check(!display.Present(picture.data(), 64, 48), "the window closed: a picture is not presented");
}

}  // namespace

int main() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    std::string error;
    std::unique_ptr<ps2gpu::Display> display =
        ps2host::platform::CreateSdlDisplay("sdl_display_tests", kWidth, kHeight, &error);
    Check(display != nullptr, "the window opens");
    if (!display) {
        std::printf("(%s)\n", error.c_str());
        return 1;
    }
    Check(display->SharedGpu() == nullptr, "no GPU behind the window");
    ThePictureIsShownAtItsAspect(*display);
    AGamepadReadsAsADualShock(*display);
    ClosingTheWindowEndsIt(*display);
    display.reset();
    SDL_Quit();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("sdl_display_tests: all passed\n");
    return 0;
}
