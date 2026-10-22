/*
 * A window through SDL 3 alone - its 2D renderer, no Vulkan - showing the
 * software GS's picture, with the keyboard and the gamepads as SDL has them.
 * What the port opens outside Windows where no Vulkan display can be had:
 * the place the GDI window has on Windows. Built where SDL is
 * (PS2HOST_VULKAN, which brings it).
 *
 * Its calls are made on the thread that opened it, the process's first: on
 * Apple's systems a window is the main thread's, and the host opens it there
 * and pumps and draws it there each frame (window.cpp, devices.cpp).
 */

#ifndef PS2HOST_SDL_DISPLAY_H
#define PS2HOST_SDL_DISPLAY_H

#include <memory>
#include <string>

#include "ps2gpu/display.h"

namespace ps2host::platform {

std::unique_ptr<ps2gpu::Display> CreateSdlDisplay(const char* title, int width, int height, std::string* error);

}  // namespace ps2host::platform

#endif  // PS2HOST_SDL_DISPLAY_H
