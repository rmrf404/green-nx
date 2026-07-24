#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gnx::stream {

// Small sequence-ordered reorder buffer for incoming Opus RTP packets.
// xCloud sends one Opus frame per RTP packet; the network can deliver them out
// of order or drop them. Opus is a stateful codec, so decoding out-of-order
// packets corrupts continuity and is audible as scratchiness. This releases
// packets to the decoder strictly in sequence order, holding briefly for a late
// packet and skipping a genuinely lost one once too many pile up behind it.
// Mirrors green-vita's SampleBuilder (32 late packets max).
class AudioJitterBuffer {
public:
    // Start a new RTP sequence space. Required between xCloud sessions because
    // the server chooses a fresh, unrelated initial sequence number.
    void reset();
    // Add one arriving Opus payload tagged with its RTP sequence number.
    void push(uint16_t seq, const uint8_t* data, size_t size);
    // Move the next in-sequence packet into `out` if one is ready; returns
    // false if we should wait for more. Call in a loop after each push().
    bool pop(std::vector<uint8_t>& out);

    // Cumulative count of packets skipped as lost (a gap we gave up waiting on).
    uint32_t lost() const { return lost_; }

private:
    struct Entry {
        uint16_t seq;
        std::vector<uint8_t> data;
    };
    // RFC 1982 serial comparison: is `a` earlier than `b` (mod 2^16)?
    static bool seq_before(uint16_t a, uint16_t b) {
        return static_cast<int16_t>(a - b) < 0;
    }

    std::vector<Entry> pending_;
    uint16_t next_seq_ = 0;
    bool have_next_ = false;
    uint32_t lost_ = 0;
};

}  // namespace gnx::stream
