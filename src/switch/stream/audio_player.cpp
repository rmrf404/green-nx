#include "audio_player.hpp"

#include <malloc.h>

#include <chrono>
#include <cstring>

namespace gnx::stream {

namespace {
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kMaxFrameSamples = 5760;  // 120 ms at 48 kHz, per channel
constexpr int kInt16PerMs = kSampleRate * kChannels / 1000;  // 96
constexpr int kRingMs = 1000;           // ring capacity
constexpr int kPrebufferMs = 80;        // fill this deep before playback starts
constexpr int kResumeMs = 40;           // shallower re-prime after an underrun
constexpr int kHighWatermarkMs = 300;   // above this we shed to bound latency
constexpr int kShedTargetMs = 150;      // ...down to here

// Clock-skew servo: the server's 48 kHz and the console's 48 kHz come from
// different crystals, so without correction the ring drains (or fills) by a
// few hundred ppm forever and eventually underruns. Steer the smoothed ring
// depth toward a target by stretching/shrinking decoded audio a fraction of a
// percent — far below audibility, but enough to null any realistic skew.
constexpr float kServoTargetMs = 100.0f;
constexpr float kServoEmaAlpha = 0.02f;      // ~1 s time constant at 50 fps
constexpr float kServoGain = 30e-6f;         // 30 ppm per ms of depth error
constexpr float kServoMaxAdj = 2000e-6f;     // clamp at +/-0.2%
constexpr int kOutBufSamples = 480;     // 10 ms per hardware buffer
constexpr size_t kOutBufBytes = kOutBufSamples * kChannels * sizeof(int16_t);
constexpr u64 kOutWaitNs = 100000000ULL;  // 100 ms; re-check quit_ on timeout
}  // namespace

bool AudioPlayer::init() {
    int error = 0;
    decoder_ = opus_decoder_create(kSampleRate, kChannels, &error);
    if (error != OPUS_OK) return false;

    ring_size_ = kRingMs * kInt16PerMs;
    ring_.assign(ring_size_, 0);
    read_ = write_ = count_ = 0;
    prime_target_ = kPrebufferMs * kInt16PerMs;
    primed_ = false;
    pcm_.resize(kMaxFrameSamples * kChannels);
    // +1% headroom: the servo can stretch a frame by at most 0.2%.
    pcm_out_.resize((kMaxFrameSamples + kMaxFrameSamples / 100) * kChannels);
    depth_ema_ = 0.0f;
    servo_adj_ = 0.0f;
    resample_pos_ = 0.0f;
    carry_[0] = carry_[1] = 0;

    // AudioPlayer is reused across streams, so clear the per-session reorder
    // buffer, inbox and counters. The reorder reset is the important one: a
    // stale next_seq_ from the previous stream makes the RFC1982 "too late"
    // check drop the whole new stream (intermittent no-audio until reconnect).
    reorder_.reset();
    inbox_.clear();
    received_ = played_ = failed_ = lost_ = 0;
    underruns_ = dropped_samples_ = frames_ = out_samples_ = 0;
    adj_ppm_ = 0;
    ema_ms_ = 0;

    if (R_FAILED(audoutInitialize())) return false;
    audout_up_ = true;
    if (R_FAILED(audoutStartAudioOut())) return false;
    device_hz_ = static_cast<int>(audoutGetSampleRate());

    // Prime the hardware queue with silence. From here on the output thread
    // refills each buffer the moment audout releases it, so >=3 buffers stay
    // queued and playback is gapless by construction.
    for (int i = 0; i < kNumOutBufs; ++i) {
        out_mem_[i] = memalign(0x1000, 0x1000);  // audout: 0x1000-aligned
        if (out_mem_[i] == nullptr) return false;
        std::memset(out_mem_[i], 0, kOutBufBytes);
        out_bufs_[i] = {};
        out_bufs_[i].buffer = out_mem_[i];
        out_bufs_[i].buffer_size = 0x1000;
        out_bufs_[i].data_size = kOutBufBytes;
        armDCacheFlush(out_mem_[i], kOutBufBytes);
        audoutAppendAudioOutBuffer(&out_bufs_[i]);
    }

    quit_ = false;
    thread_ = std::thread(&AudioPlayer::thread_main, this);
    out_thread_ = std::thread(&AudioPlayer::out_thread_main, this);
    return true;
}

void AudioPlayer::shutdown() {
    quit_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (out_thread_.joinable()) out_thread_.join();  // wakes within 100 ms
    if (audout_up_) {
        audoutStopAudioOut();
        audoutExit();
        audout_up_ = false;
    }
    for (auto& m : out_mem_) {
        free(m);
        m = nullptr;
    }
    if (decoder_) {
        opus_decoder_destroy(decoder_);
        decoder_ = nullptr;
    }
}

void AudioPlayer::submit(uint16_t seq, const uint8_t* data, size_t size) {
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        inbox_.push_back({seq, std::vector<uint8_t>(data, data + size)});
    }
    received_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
}

void AudioPlayer::thread_main() {
    std::deque<InPacket> batch;
    while (!quit_.load()) {
        {
            std::unique_lock<std::mutex> lock(inbox_mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(5),
                         [this] { return quit_.load() || !inbox_.empty(); });
            batch.swap(inbox_);
        }
        for (auto& p : batch)
            reorder_.push(p.seq, p.data.data(), p.data.size());
        batch.clear();

        std::vector<uint8_t> pkt;
        while (reorder_.pop(pkt)) {
            int samples =
                opus_decode(decoder_, pkt.data(),
                            static_cast<opus_int32>(pkt.size()), pcm_.data(),
                            kMaxFrameSamples, 0);
            if (samples <= 0) {
                failed_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            frames_.fetch_add(static_cast<uint32_t>(samples),
                              std::memory_order_relaxed);
            played_.fetch_add(1, std::memory_order_relaxed);

            // Output-volume gain + soft-knee limiter. xCloud/console audio plays
            // quiet at unity, so we lift it; but a hard clip at high gain
            // distorts loud passages. Everything below the knee stays perfectly
            // linear (normal audio is untouched); only peaks above the knee are
            // compressed smoothly toward full scale, so higher volumes stay
            // clean instead of clipping. `over/(over+headroom)` maps the
            // overshoot into the remaining headroom, so |out| never reaches the
            // rail. A cheap per-sample op; unity gain is a no-op.
            float gain = gain_.load(std::memory_order_relaxed);
            if (gain != 1.0f) {
                constexpr float kKnee = 24000.0f;               // ~ -2.7 dBFS
                constexpr float kHeadroom = 32767.0f - kKnee;   // 8767
                const int total = samples * kChannels;
                for (int i = 0; i < total; ++i) {
                    float s = pcm_[i] * gain;
                    float a = s < 0.0f ? -s : s;
                    if (a > kKnee) {
                        float over = a - kKnee;
                        a = kKnee + kHeadroom * (over / (over + kHeadroom));
                        s = s < 0.0f ? -a : a;
                    }
                    pcm_[i] = static_cast<int16_t>(s);
                }
            }

            // Steer the servo on smoothed ring depth, then stretch/shrink
            // this frame accordingly before it enters the ring.
            float depth_ms;
            {
                std::lock_guard<std::mutex> lock(ring_mutex_);
                depth_ms = static_cast<float>(count_) / kInt16PerMs;
            }
            depth_ema_ += kServoEmaAlpha * (depth_ms - depth_ema_);
            float adj = (kServoTargetMs - depth_ema_) * kServoGain;
            if (adj > kServoMaxAdj) adj = kServoMaxAdj;
            if (adj < -kServoMaxAdj) adj = -kServoMaxAdj;
            servo_adj_ = adj;
            adj_ppm_.store(static_cast<int32_t>(adj * 1e6f),
                           std::memory_order_relaxed);
            ema_ms_.store(static_cast<uint32_t>(depth_ema_),
                          std::memory_order_relaxed);

            int out_samples = servo_resample(
                pcm_.data(), samples, pcm_out_.data(),
                static_cast<int>(pcm_out_.size()) / kChannels);
            push_pcm(pcm_out_.data(), out_samples * kChannels);
        }
        lost_.store(reorder_.lost(), std::memory_order_relaxed);
    }
}

void AudioPlayer::out_thread_main() {
    // 10 ms refill deadlines: run above the render/decode threads so they
    // cannot delay us. 0x1C is the highest priority an application may use.
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x1C);

    while (!quit_.load()) {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        Result rc = audoutWaitPlayFinish(&released, &count, kOutWaitNs);
        if (R_FAILED(rc) || released == nullptr) continue;
        while (released != nullptr) {
            fill(static_cast<int16_t*>(released->buffer),
                 kOutBufSamples * kChannels);
            released->data_size = kOutBufBytes;
            armDCacheFlush(released->buffer, kOutBufBytes);
            audoutAppendAudioOutBuffer(released);
            out_samples_.fetch_add(kOutBufSamples, std::memory_order_relaxed);
            // If we were late, more than one buffer may have been released;
            // drain them all so the hardware queue refills in one pass.
            released = nullptr;
            count = 0;
            audoutGetReleasedAudioOutBuffer(&released, &count);
            if (count == 0) released = nullptr;
        }
    }
}

int AudioPlayer::servo_resample(const int16_t* in, int in_samples,
                                int16_t* out, int max_out) {
    // Read positions advance by `step` input samples per output sample, so
    // step < 1 stretches (more output) and step > 1 shrinks. The virtual
    // input stream is carry_ (the previous frame's last sample) followed by
    // this frame; pos 0.0 sits on carry_, pos 1.0 on in[0]. The fractional
    // phase carries across frames so the stretch is continuous, not stepped.
    const float step = 1.0f - servo_adj_;
    float pos = resample_pos_;
    int out_n = 0;
    while (out_n < max_out) {
        int i = static_cast<int>(pos);
        if (i >= in_samples) {
            pos -= static_cast<float>(in_samples);
            break;
        }
        float frac = pos - static_cast<float>(i);
        int16_t l0 = (i == 0) ? carry_[0] : in[(i - 1) * kChannels];
        int16_t r0 = (i == 0) ? carry_[1] : in[(i - 1) * kChannels + 1];
        int16_t l1 = in[i * kChannels];
        int16_t r1 = in[i * kChannels + 1];
        out[out_n * kChannels] =
            static_cast<int16_t>(l0 + static_cast<float>(l1 - l0) * frac);
        out[out_n * kChannels + 1] =
            static_cast<int16_t>(r0 + static_cast<float>(r1 - r0) * frac);
        ++out_n;
        pos += step;
    }
    resample_pos_ = pos;
    carry_[0] = in[(in_samples - 1) * kChannels];
    carry_[1] = in[(in_samples - 1) * kChannels + 1];
    return out_n;
}

void AudioPlayer::push_pcm(const int16_t* data, int count) {
    const int high = kHighWatermarkMs * kInt16PerMs;
    const int target = kShedTargetMs * kInt16PerMs;

    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (count > ring_size_) {  // safety; a single Opus frame is far smaller
        data += count - ring_size_;
        count = ring_size_;
    }

    // Bound latency: if this write would push the ring past the high watermark,
    // drop the oldest samples down to the target. A small, occasional skip that
    // keeps audio close to live instead of drifting steadily late.
    if (count_ + count > high) {
        int shed = (count_ + count) - target;
        if (shed > count_) shed = count_;
        if (shed > 0) {
            read_ = (read_ + shed) % ring_size_;
            count_ -= shed;
            dropped_samples_.fetch_add(static_cast<uint32_t>(shed),
                                       std::memory_order_relaxed);
        }
    }
    // Hard safety: never exceed capacity.
    if (count_ + count > ring_size_) {
        int over = (count_ + count) - ring_size_;
        read_ = (read_ + over) % ring_size_;
        count_ -= over;
        dropped_samples_.fetch_add(static_cast<uint32_t>(over),
                                   std::memory_order_relaxed);
    }

    int first = ring_size_ - write_;
    if (first > count) first = count;
    std::memcpy(&ring_[write_], data, first * sizeof(int16_t));
    if (count > first)
        std::memcpy(&ring_[0], data + first,
                    (count - first) * sizeof(int16_t));
    write_ = (write_ + count) % ring_size_;
    count_ += count;

    if (!primed_ && count_ >= prime_target_) primed_ = true;
}

void AudioPlayer::fill(int16_t* out, int need) {
    int given = 0;
    bool underran = false;
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        if (primed_) {
            int give = need < count_ ? need : count_;
            int first = ring_size_ - read_;
            if (first > give) first = give;
            std::memcpy(out, &ring_[read_], first * sizeof(int16_t));
            if (give > first)
                std::memcpy(out + first, &ring_[0],
                            (give - first) * sizeof(int16_t));
            read_ = (read_ + give) % ring_size_;
            count_ -= give;
            given = give;
            if (given < need) {
                // Ran dry: resume as soon as a shallow cushion exists rather
                // than demanding a full re-prime; the servo then rebuilds
                // depth to target gradually and inaudibly.
                primed_ = false;
                prime_target_ = kResumeMs * kInt16PerMs;
                underran = true;
            }
        }
    }
    if (given < need)
        std::memset(out + given, 0, (need - given) * sizeof(int16_t));
    if (underran) underruns_.fetch_add(1, std::memory_order_relaxed);
}

AudioPlayer::Stats AudioPlayer::stats() const {
    Stats s;
    s.received = received_.load(std::memory_order_relaxed);
    s.played = played_.load(std::memory_order_relaxed);
    s.failed = failed_.load(std::memory_order_relaxed);
    s.lost = lost_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.dropped_ms = dropped_samples_.load(std::memory_order_relaxed) / kInt16PerMs;
    s.frames = frames_.load(std::memory_order_relaxed);
    s.out_samples = out_samples_.load(std::memory_order_relaxed);
    s.ema_ms = ema_ms_.load(std::memory_order_relaxed);
    s.adj_ppm = adj_ppm_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        s.queue_ms = count_ / kInt16PerMs;
    }
    return s;
}

}  // namespace gnx::stream
