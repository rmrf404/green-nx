#include "video_decoder.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace gnx::stream {

namespace {

#ifdef __SWITCH__
enum AVPixelFormat pick_hw_format(AVCodecContext* context,
                                  const enum AVPixelFormat* formats) {
    for (const enum AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_NVTEGRA) return *p;
    }
    (void)context;
    return formats[0];  // software fallback
}
#endif

}  // namespace

bool VideoDecoder::init(SDL_Renderer* renderer) {
    renderer_ = renderer;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return false;
    context_ = avcodec_alloc_context3(codec);
    if (!context_) return false;

    context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context_->flags2 |= AV_CODEC_FLAG2_FAST;

#ifdef __SWITCH__
    if (av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_NVTEGRA, nullptr,
                               nullptr, 0) == 0) {
        context_->hw_device_ctx = av_buffer_ref(hw_device_);
        context_->get_format = pick_hw_format;
    } else {
        std::fprintf(stderr, "nvtegra unavailable, software decode\n");
        context_->thread_type = FF_THREAD_SLICE;
        context_->thread_count = 3;
    }
#else
    context_->thread_count = 4;
#endif

    if (avcodec_open2(context_, codec, nullptr) != 0) return false;
    frame_ = av_frame_alloc();
    held_frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    return frame_ && held_frame_ && packet_;
}

void VideoDecoder::shutdown() {
    if (packet_) av_packet_free(&packet_);
    if (frame_) av_frame_free(&frame_);
    if (held_frame_) av_frame_free(&held_frame_);
    if (context_) avcodec_free_context(&context_);
    if (hw_device_) av_buffer_unref(&hw_device_);
    if (texture_) SDL_DestroyTexture(texture_), texture_ = nullptr;
}

bool VideoDecoder::ensure_texture(SDL_Renderer* renderer, int width,
                                  int height) {
    if (texture_ && width == width_ && height == height_) return true;
    if (texture_) SDL_DestroyTexture(texture_);
    texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                 SDL_TEXTUREACCESS_STREAMING, width, height);
    width_ = width;
    height_ = height;
    return texture_ != nullptr;
}

bool VideoDecoder::decode(const uint8_t* data, size_t size) {
    av_packet_unref(packet_);
    if (av_new_packet(packet_, static_cast<int>(size)) != 0) return false;
    std::memcpy(packet_->data, data, size);

    if (avcodec_send_packet(context_, packet_) != 0) {
        error_ = true;
        return false;
    }

    bool rendered = false;
    while (avcodec_receive_frame(context_, frame_) == 0) {
        if (frame_->decode_error_flags != 0 ||
            (frame_->flags & AV_FRAME_FLAG_CORRUPT))
            error_ = true;
#ifdef __SWITCH__
        // Zero-copy: keep the raw NVTEGRA surface for the deko3d renderer -- no
        // GPU->CPU transfer, no software colour conversion (both of which
        // corrupt on the Switch SDL path). We must ref it into held_frame_:
        // the next avcodec_receive_frame() unrefs frame_ (leaving fmt=-1), so
        // rendering frame_ after decode() returns would see an empty frame.
        const char* name = av_get_pix_fmt_name(
            static_cast<AVPixelFormat>(frame_->format));
        last_pixfmt_ = name ? name : "?";
        width_ = frame_->width;
        height_ = frame_->height;
        av_frame_unref(held_frame_);
        if (av_frame_ref(held_frame_, frame_) == 0) rendered = true;
#else
        if (upload(frame_)) rendered = true;
#endif
    }
    return rendered;
}

bool VideoDecoder::upload(AVFrame* frame) {
    const int w = frame->width, h = frame->height;
    if (w <= 0 || h <= 0) return false;
    if (!ensure_texture(renderer_, w, h)) return false;

    const char* name = av_get_pix_fmt_name(
        static_cast<AVPixelFormat>(frame->format));
    last_pixfmt_ = name ? name : "?";

    // Build a tightly-packed I420 image (Y plane, then U, then V), copying row
    // by row so the source stride (which is often padded/aligned) is handled
    // explicitly. Then hand SDL exact, tightly-packed pitches -- this avoids
    // both SDL's fragile NV12 path and any stride mismatch.
    const int cw = w / 2, ch = h / 2;
    i420_.resize(static_cast<size_t>(w) * h + 2 * static_cast<size_t>(cw) * ch);
    uint8_t* y = i420_.data();
    uint8_t* u = y + static_cast<size_t>(w) * h;
    uint8_t* v = u + static_cast<size_t>(cw) * ch;

    for (int r = 0; r < h; ++r)
        std::memcpy(y + static_cast<size_t>(r) * w,
                    frame->data[0] + static_cast<size_t>(r) * frame->linesize[0],
                    w);

    if (frame->format == AV_PIX_FMT_NV12) {
        // Interleaved UV -> separate U and V planes.
        for (int r = 0; r < ch; ++r) {
            const uint8_t* uv =
                frame->data[1] + static_cast<size_t>(r) * frame->linesize[1];
            uint8_t* ud = u + static_cast<size_t>(r) * cw;
            uint8_t* vd = v + static_cast<size_t>(r) * cw;
            for (int c = 0; c < cw; ++c) {
                ud[c] = uv[2 * c];
                vd[c] = uv[2 * c + 1];
            }
        }
    } else if (frame->format == AV_PIX_FMT_YUV420P ||
               frame->format == AV_PIX_FMT_YUVJ420P) {
        for (int r = 0; r < ch; ++r) {
            std::memcpy(u + static_cast<size_t>(r) * cw,
                        frame->data[1] + static_cast<size_t>(r) * frame->linesize[1], cw);
            std::memcpy(v + static_cast<size_t>(r) * cw,
                        frame->data[2] + static_cast<size_t>(r) * frame->linesize[2], cw);
        }
    } else {
        return false;
    }

    return SDL_UpdateYUVTexture(texture_, nullptr, y, w, u, cw, v, cw) == 0;
}

}  // namespace gnx::stream
