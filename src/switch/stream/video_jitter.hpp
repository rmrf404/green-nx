#pragma once

#include <SDL2/SDL.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

namespace gnx::stream {

// RTP -> H.264 access-unit reassembly with a proper time-delayed jitter buffer.
// Modeled on green-vita's VideoRtp + the reorder buffer its rtc stack provides.
//
// Why the delay matters: a naive "assemble the current frame, drop it the
// instant the next timestamp arrives" buffer drops any frame that lost a packet
// BEFORE the NACK retransmit can arrive (~1 RTT), then keeps feeding the decoder
// P-frames that reference the dropped frame -> the decoder produces garbage
// (green/pink) or stalls. This buffer instead:
//   * holds frames a short time (kHoldMs) so retransmits/reordering complete them,
//   * emits complete frames strictly in timestamp (decode) order,
//   * on unrecoverable loss (a frame times out, or a whole frame is missing),
//     drops and refuses to emit anything until a fresh IDR -- no broken P-frames
//     ever reach the decoder.
class VideoJitterBuffer {
public:
    using Emit = std::function<void(const uint8_t* data, size_t size)>;
    using NackFn = std::function<void(uint16_t pid, uint16_t blp)>;

    void reset();

    // Feed one decrypted RTP packet at time now_ms (SDL_GetTicks64).
    void receive(const uint8_t* rtp, size_t len, uint64_t now_ms,
                 const Emit& emit, const NackFn& nack, bool* want_keyframe);

    bool report_stats(uint8_t* fraction_lost, uint32_t* cumulative_lost,
                      uint32_t* highest_seq_ext);

    struct Stats {
        uint32_t packets = 0, frames = 0, dropped = 0, nacks = 0, resyncs = 0;
        uint32_t last_frame_bytes = 0;
    };
    Stats stats() const { return stats_; }
    // True while dropping everything until a real IDR arrives.
    bool waiting_keyframe() const { return waiting_keyframe_; }

private:
    struct Packet {
        uint16_t seq = 0;
        bool marker = false;
        std::vector<uint8_t> payload;
    };
    struct Frame {
        uint32_t timestamp = 0;
        uint64_t first_seen_ms = 0;
        std::vector<Packet> packets;
    };

    Frame* find_or_create(uint32_t ts, uint64_t now_ms);
    bool try_assemble(Frame& f, std::vector<uint8_t>& au, uint16_t* marker_seq);

    std::deque<Frame> frames_;         // pending frames, ascending timestamp
    std::optional<uint32_t> last_ts_;  // timestamp of the last emitted frame
    bool waiting_keyframe_ = true;

    // Global sequence tracking for NACK + receiver-report stats.
    bool have_seq_ = false;
    uint16_t max_seq_ = 0;
    uint32_t cycles_ = 0, base_seq_ = 0, received_ = 0;
    uint32_t received_prior_ = 0, expected_prior_ = 0;

    Stats stats_;
};

}  // namespace gnx::stream
