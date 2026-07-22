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

#include <cstdarg>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <deko3d.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace gnx::stream {

class DkVideoRenderer {
public:
    using LogFn = std::function<void(const char*)>;

    DkVideoRenderer() = default;
    ~DkVideoRenderer();

    DkVideoRenderer(const DkVideoRenderer&) = delete;
    DkVideoRenderer& operator=(const DkVideoRenderer&) = delete;

    void set_logger(LogFn fn) { log_ = std::move(fn); }

    // Bring up the deko3d device/swapchain. Call after SDL has released the
    // window. Returns false (and logs) on failure.
    bool init();
    void shutdown();

    // Present one decoded frame. frame must be AV_PIX_FMT_NVTEGRA. Returns
    // false if the frame could not be rendered.
    bool render(AVFrame* frame);

    bool initialized() const { return initialized_; }

private:
    static constexpr unsigned kFbNum = 2;
    static constexpr uint32_t kFbWidth = 1280;
    static constexpr uint32_t kFbHeight = 720;

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
    // (Re)build the R8/RG8 layouts on the surface's REAL allocated geometry
    // (aligned dims, pitch vs block linear); crop to the visible area in the
    // shader via the transform.
    bool ensure_layouts(AVFrame* frame, bool is_linear);
    FrameMapping* map_frame(AVFrame* frame, void* base, uint32_t handle,
                            uint32_t size);  // zero-copy import (cached)
    void update_transform(AVFrame* frame);

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
    int color_space_ = -1;
    bool color_full_ = false;
    bool warned_not_hw_ = false;
    bool logged_surface_ = false;
};

}  // namespace gnx::stream
