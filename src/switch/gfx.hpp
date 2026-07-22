#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <string>
#include <unordered_map>

namespace gnx::gfx {

// Design-space resolution; SDL logical size scales it to 720p/1080p output.
constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

struct Color {
    Uint8 r, g, b, a = 255;
};

// Palette
constexpr Color kBg{16, 18, 24};
constexpr Color kBgAccent{24, 28, 38};
constexpr Color kCard{34, 39, 52};
constexpr Color kCardFocus{16, 124, 16};  // Xbox green
constexpr Color kText{235, 238, 245};
constexpr Color kTextDim{150, 156, 170};
constexpr Color kAccent{16, 124, 16};
constexpr Color kWarn{240, 180, 60};
constexpr Color kError{230, 90, 90};

enum class FontSize { Small = 0, Body, Title, Huge, Mono };

class Gfx {
public:
    bool init();
    void shutdown();

    SDL_Renderer* renderer() { return renderer_; }

    // Release the SDL window/renderer so deko3d can take over the single Switch
    // display during streaming; resume() rebuilds them afterwards. Cached
    // textures are destroyed with the renderer and regenerate on demand.
    void suspend();
    bool resume();

    void begin_frame();
    void end_frame();

    void fill(const SDL_Rect& rect, Color color);
    void frame(const SDL_Rect& rect, Color color, int thickness = 3);
    // Text draws return the rendered width.
    int text(const std::string& utf8, int x, int y, FontSize size, Color color);
    int text_centered(const std::string& utf8, int cx, int y, FontSize size,
                      Color color);
    int text_width(const std::string& utf8, FontSize size);

    SDL_Texture* texture_from_memory(const void* data, size_t size);
    void draw_texture(SDL_Texture* texture, const SDL_Rect& destination);

    // Simple pulsing loading dot row.
    void spinner(int cx, int y, Uint32 ticks);

private:
    TTF_Font* font(FontSize size);
    SDL_Texture* render_text(const std::string& utf8, FontSize size,
                             Color color, int* width, int* height);
    bool create_window_renderer();  // window + renderer only (no subsystem init)

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    TTF_Font* fonts_[5] = {};
    void* font_data_ = nullptr;  // shared system font blob (not owned)

    struct CachedText {
        SDL_Texture* texture;
        int width, height;
        Uint32 last_used;
    };
    std::unordered_map<std::string, CachedText> text_cache_;
    void trim_text_cache();
};

}  // namespace gnx::gfx
