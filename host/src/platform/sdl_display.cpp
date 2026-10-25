/*
 * The plain SDL window (sdl_display.h): SDL's 2D renderer draws the picture
 * the host hands it, scaled to the window at the picture's aspect; the
 * keyboard and gamepads are read as the Vulkan display reads them
 * (ps2gpu/sdl_pad.h). No overlay, cursor art, mouse or trigger effects:
 * those are the Vulkan display's.
 */

#include "sdl_display.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ps2gpu/sdl_pad.h"

namespace ps2host::platform {

namespace {

class SdlDisplay final : public ps2gpu::Display {
public:
    ~SdlDisplay() override {
        for (Gamepad& g : pads_) SDL_CloseGamepad(g.pad);
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
    }

    bool Open(const char* title, int width, int height, std::string* error) {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return Fail(error);
        window_ = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window_) return Fail(error);
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (!renderer_) return Fail(error);
        SDL_SetRenderVSync(renderer_, 0);           // the host paces its pictures itself
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
            for (int i = 0; i < count; ++i) AddPad(ids[i]);
            SDL_free(ids);
        }
        return true;
    }

    bool Present(const std::uint32_t* pixels, std::uint32_t width, std::uint32_t height) override {
        if (closed_ || !pixels || !width || !height) return !closed_;
        if (!texture_ || width != texture_width_ || height != texture_height_) {
            if (texture_) SDL_DestroyTexture(texture_);
            // The picture's 32-bit BGRA is XRGB8888 read as a word.
            texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                         int(width), int(height));
            if (!texture_) return !closed_;
            SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
            texture_width_ = width;
            texture_height_ = height;
        }
        SDL_UpdateTexture(texture_, nullptr, pixels, int(width * 4));
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        int out_width = 0, out_height = 0;
        SDL_GetRenderOutputSize(renderer_, &out_width, &out_height);
        // The picture at its aspect (the "aspect" setting, else the window's
        // shape), as large as the window holds, centred.
        const float aspect = aspect_ > 0.0f ? aspect_ : float(out_width) / float(std::max(1, out_height));
        float w = float(out_width), h = w / aspect;
        if (h > float(out_height)) {
            h = float(out_height);
            w = h * aspect;
        }
        const SDL_FRect to{(float(out_width) - w) / 2, (float(out_height) - h) / 2, w, h};
        SDL_RenderTexture(renderer_, texture_, nullptr, &to);
        SDL_RenderPresent(renderer_);
        ++presents_;
        return !closed_;
    }

    bool Pump() override {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    if (!closed_) {
                        closed_by_ = event.type == SDL_EVENT_QUIT
                                         ? "the app's Quit (Cmd+Q, or Quit in its menu)"
                                         : "the window's close (its button, or Cmd+W)";
                        std::fprintf(stderr, "[window] closed by %s\n", closed_by_.c_str());
                    }
                    closed_ = true;
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    AddPad(event.gdevice.which);
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    RemovePad(event.gdevice.which);
                    break;
                case SDL_EVENT_KEY_DOWN:
                    // Alt+Enter (Option+Return): fullscreen on or off, as the Vulkan display has it.
                    if (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT) && !event.key.repeat)
                        SetFullscreen(!Fullscreen());
                    break;
                default:
                    break;
            }
        }
        return !closed_;
    }

    std::string ClosedBy() const override { return closed_ ? closed_by_ : std::string(); }

    bool Focused() const override {
        return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
    }

    bool KeyDown(int scancode) const override {
        int n = 0;
        const bool* keys = SDL_GetKeyboardState(&n);
        return keys && scancode >= 0 && scancode < n && keys[scancode];
    }

    bool Pad(PadState* state) const override {
        if (pads_.empty()) return false;
        *state = ps2gpu::ReadSdlPad(pads_.front().pad);
        return true;
    }

    std::vector<ControllerInfo> Controllers() const override {
        std::vector<ControllerInfo> list;
        for (const Gamepad& g : pads_) {
            const char* name = SDL_GetGamepadName(g.pad);
            list.push_back({g.id, name ? name : "Controller", ps2gpu::SdlPadFamily(g.pad)});
        }
        return list;
    }

    bool PadOf(std::uint32_t id, PadState* state) const override {
        for (const Gamepad& g : pads_)
            if (g.id == id) {
                *state = ps2gpu::ReadSdlPad(g.pad);
                return true;
            }
        return false;
    }

    void SetRumble(std::uint16_t large, std::uint16_t small) override {
        if (!pads_.empty()) SDL_RumbleGamepad(pads_.front().pad, large, small, 1000);
    }

    void SetRumbleOf(std::uint32_t id, std::uint16_t large, std::uint16_t small) override {
        for (Gamepad& g : pads_)
            if (g.id == id) SDL_RumbleGamepad(g.pad, large, small, 1000);
    }

    void ReleaseController() override {
        for (Gamepad& g : pads_) SDL_RumbleGamepad(g.pad, 0, 0, 0);
    }

    void SetTitle(const std::string& title) override { SDL_SetWindowTitle(window_, title.c_str()); }
    void SetFullscreen(bool on) override { SDL_SetWindowFullscreen(window_, on); }
    bool Fullscreen() const override { return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0; }
    void SetAspect(float aspect) override { aspect_ = aspect; }

    float WindowAspect() const override {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        return h > 0 ? float(w) / float(h) : 0.0f;
    }

    std::uint32_t PixelHeight() const override {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        return std::uint32_t(std::max(0, h));
    }

    std::string Describe() const override {
        const char* name = renderer_ ? SDL_GetRendererName(renderer_) : nullptr;
        return std::string("SDL's ") + (name ? name : "2D") + " renderer drawing the software GS's picture";
    }

    ps2gpu::Gpu* SharedGpu() override { return nullptr; }

private:
    struct Gamepad {
        std::uint32_t id = 0;
        SDL_Gamepad* pad = nullptr;
    };

    bool Fail(std::string* error) {
        if (error) *error = SDL_GetError();
        return false;
    }

    void AddPad(SDL_JoystickID id) {
        for (const Gamepad& g : pads_)
            if (g.id == id) return;
        if (SDL_Gamepad* pad = SDL_OpenGamepad(id)) pads_.push_back({std::uint32_t(id), pad});
    }

    void RemovePad(SDL_JoystickID id) {
        for (auto it = pads_.begin(); it != pads_.end(); ++it)
            if (it->id == id) {
                SDL_CloseGamepad(it->pad);
                pads_.erase(it);
                return;
            }
    }

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    std::uint32_t texture_width_ = 0, texture_height_ = 0;
    std::vector<Gamepad> pads_;
    float aspect_ = 4.0f / 3.0f;
    bool closed_ = false;
    std::string closed_by_;             // ClosedBy
    std::uint64_t presents_ = 0;
};

}  // namespace

std::unique_ptr<ps2gpu::Display> CreateSdlDisplay(const char* title, int width, int height, std::string* error) {
    auto display = std::make_unique<SdlDisplay>();
    if (!display->Open(title, width, height, error)) return nullptr;
    return display;
}

}  // namespace ps2host::platform
