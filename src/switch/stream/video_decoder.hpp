#pragma once

#include <SDL2/SDL.h>

#include <cstdint>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace gnx::stream {

// H.264 decoder: NVDEC (nvtegra hwaccel) on Switch, software elsewhere.
// Input: complete Annex-B access units from the RTP depacketizer.
// Output: a tightly-packed I420 SDL texture (SDL_PIXELFORMAT_IYUV). We convert
// NV12/YUV420P to packed I420 ourselves rather than trust SDL's NV12 path or
// pass padded strides -- both produce green/pink corruption on the Switch port.
class VideoDecoder {
public:
    bool init(SDL_Renderer* renderer);
    void shutdown();

    // Feed one access unit; returns true if a new frame was rendered into
    // the texture.
    bool decode(const uint8_t* data, size_t size);

    // True (and reset) if a decode error/corrupt frame was seen since the last
    // call -- the caller should ask the server for a fresh keyframe.
    bool take_error() {
        bool e = error_;
        error_ = false;
        return e;
    }

    SDL_Texture* texture() { return texture_; }
    // Latest decoded frame. On Switch this is a held reference to the raw
    // NVTEGRA hardware surface (kept un-transferred for zero-copy deko3d
    // rendering, and ref'd so it survives the next avcodec_receive_frame which
    // would otherwise unref frame_).
    AVFrame* current_frame() { return held_frame_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool hardware() const { return hw_device_ != nullptr; }
    // Name of the last decoded frame's pixel format (diagnostics).
    const char* last_pixfmt() const { return last_pixfmt_; }

private:
    bool ensure_texture(SDL_Renderer* renderer, int width, int height);
    bool upload(AVFrame* frame);

    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int width_ = 0, height_ = 0;

    AVCodecContext* context_ = nullptr;
    AVBufferRef* hw_device_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* held_frame_ = nullptr;  // Switch: surviving ref of the last frame
    AVPacket* packet_ = nullptr;
    bool error_ = false;
    const char* last_pixfmt_ = "?";
    std::vector<uint8_t> i420_;  // scratch for packed I420 upload
};

}  // namespace gnx::stream
