#pragma once

#include <SDL2/SDL.h>

#include <cstdint>
#include <vector>

#include <opus/opus.h>

namespace gnx::stream {

// Opus 48 kHz stereo -> SDL audio queue.
class AudioPlayer {
public:
    bool init();
    void shutdown();

    // One Opus packet from the RTP depacketizer.
    void play(const uint8_t* data, size_t size);

    // Telemetry (diagnostics): decoded packets, decode failures, overflow drops.
    struct Stats {
        uint32_t played = 0, failed = 0, cleared = 0;
    };
    Stats stats() const { return stats_; }

private:
    OpusDecoder* decoder_ = nullptr;
    SDL_AudioDeviceID device_ = 0;
    std::vector<int16_t> pcm_;
    Stats stats_;
};

}  // namespace gnx::stream
