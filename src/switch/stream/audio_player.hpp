#pragma once

#include <switch.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <opus/opus.h>

#include "audio_jitter.hpp"

namespace gnx::stream {

// Opus 48 kHz stereo -> libnx audout playback.
//
// submit() (network thread) only copies the packet into an inbox. A decode
// thread reorders and decodes into a PCM ring, and a high-priority output
// thread feeds the console's audio-out service (audout) from that ring,
// keeping several hardware buffers queued at all times so the device never
// runs out of queued audio between refills.
//
// audout is fed directly instead of through SDL because SDL2's Switch audio
// backend (audren-based) queues a single buffer and then busy-waits — in 5 ms
// audio-renderer frames — for that buffer to *start* playing before it will
// accept more data. Every buffer cycle therefore rounds up to the next 5 ms
// boundary: the device consumes audio slower than real time (~85% measured;
// the ring shed ~160 ms/s while perfectly healthy) and the voice starves for
// 5 ms whenever a cycle loses the race. That is unfixable from above the SDL
// API, no matter how the application buffers.
class AudioPlayer {
public:
    bool init();
    void shutdown();

    // Hand one Opus RTP payload (tagged with its RTP sequence) to the audio
    // thread. Returns immediately; safe to call from the network thread.
    void submit(uint16_t seq, const uint8_t* data, size_t size);

    int device_hz() const { return device_hz_; }

    // Diagnostic telemetry (cumulative unless noted).
    struct Stats {
        uint32_t received = 0;     // packets handed to submit()
        uint32_t played = 0;       // packets decoded + buffered
        uint32_t failed = 0;       // opus decode failures
        uint32_t lost = 0;         // sequence gaps the reorder buffer skipped
        uint32_t underruns = 0;    // ring ran dry -> emitted silence
        uint32_t dropped_ms = 0;   // audio shed to bound latency (ms)
        uint32_t queue_ms = 0;     // current ring depth (ms)
        uint32_t frames = 0;       // decoded samples per channel (rate calc)
        uint32_t out_samples = 0;  // samples per channel queued to audout
        uint32_t ema_ms = 0;       // smoothed ring depth the servo steers on
        int32_t adj_ppm = 0;       // current servo stretch (+) / shrink (-)
    };
    Stats stats() const;

private:
    void thread_main();      // reorder + decode -> ring
    void out_thread_main();  // ring -> audout hardware buffers
    // Depth-servo resampler: stretches/shrinks one decoded frame by the servo
    // ratio (linear interpolation, phase carried across frames). Returns the
    // number of output samples per channel.
    int servo_resample(const int16_t* in, int in_samples, int16_t* out,
                       int max_out);
    void push_pcm(const int16_t* data, int count);  // count = int16 samples
    void fill(int16_t* out, int need);              // ring -> one out buffer

    static constexpr int kNumOutBufs = 4;

    OpusDecoder* decoder_ = nullptr;
    int device_hz_ = 0;
    bool audout_up_ = false;
    void* out_mem_[kNumOutBufs] = {};
    AudioOutBuffer out_bufs_[kNumOutBufs] = {};
    std::vector<int16_t> pcm_;   // decode scratch (decode thread only)
    std::vector<int16_t> pcm_out_;  // resampled scratch (decode thread only)
    AudioJitterBuffer reorder_;  // decode thread only

    // Clock-skew servo state (decode thread only, except the atomics).
    float depth_ema_ = 0.0f;    // smoothed ring depth in ms
    float servo_adj_ = 0.0f;    // current stretch ratio - 1 (e.g. +0.0005)
    float resample_pos_ = 0.0f; // fractional read phase into the next frame
    int16_t carry_[2] = {0, 0}; // previous frame's last L/R for interpolation
    std::atomic<int32_t> adj_ppm_{0};
    std::atomic<uint32_t> ema_ms_{0};

    // Ring buffer: producer = decode thread, consumer = output thread. All
    // ring fields are guarded by ring_mutex_.
    mutable std::mutex ring_mutex_;
    std::vector<int16_t> ring_;
    int ring_size_ = 0;
    int read_ = 0;
    int write_ = 0;
    int count_ = 0;
    int prime_target_ = 0;  // ring depth needed before playback (re)starts
    bool primed_ = false;

    std::thread thread_;
    std::thread out_thread_;
    std::atomic<bool> quit_{false};
    std::mutex inbox_mutex_;
    std::condition_variable cv_;
    struct InPacket {
        uint16_t seq;
        std::vector<uint8_t> data;
    };
    std::deque<InPacket> inbox_;

    std::atomic<uint32_t> received_{0};
    std::atomic<uint32_t> played_{0};
    std::atomic<uint32_t> failed_{0};
    std::atomic<uint32_t> lost_{0};
    std::atomic<uint32_t> underruns_{0};
    std::atomic<uint32_t> dropped_samples_{0};
    std::atomic<uint32_t> frames_{0};
    std::atomic<uint32_t> out_samples_{0};
};

}  // namespace gnx::stream
