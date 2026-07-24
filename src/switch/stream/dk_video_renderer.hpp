#pragma once

// Standalone deko3d video renderer for the Switch. Renders NVTEGRA hardware
// decoder surfaces ZERO-COPY (the decoder's GPU buffer is imported directly as
// deko3d luma/chroma textures and colour-converted by a shader) -- the same
// approach Moonlight-Switch uses. This replaces the SDL texture path, which
// corrupts (green/pink) on the Switch GL renderer.
//
// The Switch has a single display window, so SDL and deko3d cannot both own it.
// The owner suspends SDL (Gfx::suspend) before init() and resumes it after
// shutdown(); during streaming deko3d owns the framebuffer exclusively.

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <deko3d.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

// Forward-declared to keep SDL_ttf out of this header (it's a plain pointer
// member here; the full type comes from SDL_ttf.h in the .cpp).
struct _TTF_Font;
typedef struct _TTF_Font TTF_Font;

namespace gnx::stream {

class DkVideoRenderer {
public:
    using LogFn = std::function<void(const char*)>;

    DkVideoRenderer() = default;
    ~DkVideoRenderer();

    DkVideoRenderer(const DkVideoRenderer&) = delete;
    DkVideoRenderer& operator=(const DkVideoRenderer&) = delete;

    void set_logger(LogFn fn) { log_ = std::move(fn); }

    // Luma sharpening level for the fragment shader: 0=Off, 1=Low, 2=Medium,
    // 3=High. Set before init(); fixed for the stream's lifetime.
    void set_sharpness(int level) {
        sharpness_ = level < 0 ? 0 : (level > 3 ? 3 : level);
    }

    // Enable/disable the debug HUD overlay pass (drawn on top of the video).
    void set_hud_enabled(bool e) { hud_enabled_ = e; }

    // Live network stats for the HUD, fed once per second from the streaming
    // worker thread (stored atomically; read on the render thread).
    void set_net_stats(float mbps, float loss_pct, int buffer_ms) {
        net_mbps_.store(mbps, std::memory_order_relaxed);
        net_loss_.store(loss_pct, std::memory_order_relaxed);
        net_buffer_ms_.store(buffer_ms, std::memory_order_relaxed);
        net_valid_.store(true, std::memory_order_relaxed);
    }

    // Bring up the deko3d device/swapchain. Call after SDL has released the
    // window. Returns false (and logs) on failure.
    bool init();
    void shutdown();

    // Present one decoded frame. frame must be AV_PIX_FMT_NVTEGRA. Returns
    // false if the frame could not be rendered.
    bool render(AVFrame* frame);

    bool initialized() const { return initialized_; }

private:
    // Triple-buffered: with the ~60 Hz software present pacer (Engine::pump_video)
    // an extra framebuffer gives slack so a bunched present never finds the
    // swapchain empty (which makes deko3d's acquireImage abort the process).
    static constexpr unsigned kFbNum = 3;
    // Framebuffer size tracks the console's output: 720p handheld, 1080p docked.
    // Rebuilt on dock/undock so a docked TV renders a native 1080p target instead
    // of a 720p buffer the compositor then upscales. libnx defaults the nwindow
    // swap interval to 1, so the flip is already vsync-locked / tear-free.
    uint32_t fb_width_ = 1280;
    uint32_t fb_height_ = 720;
    int fb_mode_ = -1;  // AppletOperationMode the current swapchain was built for

    struct FrameMapping {
        uint32_t handle = 0;
        void* cpu_addr = nullptr;
        uint32_t size = 0;
        uint32_t luma_offset = 0;
        uint32_t chroma_offset = 0;
        dk::UniqueMemBlock memblock;
        dk::Image luma;
        dk::Image chroma;
        dk::ImageDescriptor luma_desc;
        dk::ImageDescriptor chroma_desc;
    };

    void logf(const char* fmt, ...);
    bool create_swapchain();          // (re)build framebuffers + swapchain
    void destroy_swapchain();         // waitIdle + release them
    void maybe_rebuild_swapchain();   // rebuild if the dock mode changed
    // (Re)build the R8/RG8 layouts on the surface's REAL allocated geometry
    // (aligned dims, pitch vs block linear); crop to the visible area in the
    // shader via the transform.
    bool ensure_layouts(AVFrame* frame, bool is_linear);
    FrameMapping* map_frame(AVFrame* frame, void* base, uint32_t handle,
                            uint32_t size);  // zero-copy import (cached)
    void update_transform(AVFrame* frame);
    void update_hud(AVFrame* frame);   // recompute stats, re-rasterize on change
    void rasterize_hud();              // compose panel bg + text into hud_cpu_
    void blit_text(const char* s, int x, int y);  // white text onto hud_pixels_

    LogFn log_;
    bool initialized_ = false;

    dk::UniqueDevice dev_;
    dk::UniqueQueue queue_;
    dk::UniqueCmdBuf cmdbuf_;
    dk::UniqueSwapchain swapchain_;

    // deko3d memory blocks (raw; freed in shutdown()).
    dk::UniqueMemBlock fb_memblock_;
    dk::UniqueMemBlock code_memblock_;
    dk::UniqueMemBlock cmd_memblock_;
    dk::UniqueMemBlock data_memblock_;

    dk::Image framebuffers_[kFbNum];

    dk::Shader vertex_shader_;
    dk::Shader fragment_shader_;
    dk::Shader hud_fsh_;  // HUD overlay (stage 0: flat panel; stage 1: text)

    // data_memblock_ sub-allocations (offsets):
    uint32_t vtx_offset_ = 0;        // quad vertex buffer
    uint32_t uniform_offset_ = 0;    // Transformation UBO
    uint32_t sampler_desc_offset_ = 0;
    uint32_t image_desc_offset_ = 0;
    DkGpuAddr data_gpu_ = 0;
    void* data_cpu_ = nullptr;

    dk::ImageLayout luma_layout_;
    dk::ImageLayout chroma_layout_;

    std::vector<FrameMapping> mappings_;
    int current_mapping_ = -1;

    DkSamplerDescriptor sampler_desc_{};

    int frame_w_ = 0, frame_h_ = 0;      // visible dims
    uint32_t luma_w_ = 0, luma_h_ = 0;   // aligned (allocated) dims
    uint32_t chroma_w_ = 0, chroma_h_ = 0;
    bool linear_ = false;                // pitch-linear surface?
    bool transform_dirty_ = true;
    int sharpness_ = 0;  // 0=Off..3=High, baked into the UBO (set_sharpness)
    int color_space_ = -1;
    bool color_full_ = false;
    bool warned_not_hw_ = false;
    bool logged_surface_ = false;
    bool hud_enabled_ = true;  // draw the debug HUD overlay pass

    // Debug HUD text (stage 1): stats composited on the CPU into a pitch-linear
    // RGBA texture, sampled by hud_fsh_ in the overlay pass.
    static constexpr uint32_t kHudTexW = 512;
    static constexpr uint32_t kHudTexH = 160;
    TTF_Font* hud_font_ = nullptr;
    dk::UniqueMemBlock hud_memblock_;
    dk::Image hud_image_;
    dk::ImageDescriptor hud_desc_;
    void* hud_cpu_ = nullptr;
    std::vector<uint32_t> hud_pixels_;   // CPU compose buffer (kHudTexW*kHudTexH)
    std::string hud_text_cache_;         // last rasterized text (skip if unchanged)
    uint64_t fps_tick_ = 0;              // armGetSystemTick at last FPS sample
    int fps_frames_ = 0;
    float fps_ = 0.0f;
    std::atomic<float> net_mbps_{0.0f};   // set by Engine worker, read by update_hud
    std::atomic<float> net_loss_{0.0f};
    std::atomic<int> net_buffer_ms_{0};
    std::atomic<bool> net_valid_{false};
};

}  // namespace gnx::stream
