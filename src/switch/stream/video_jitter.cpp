#include "video_jitter.hpp"

#include <algorithm>
#include <cstring>

namespace gnx::stream {

namespace {

constexpr size_t kMaxAccessUnitBytes = 2 * 1024 * 1024;
constexpr uint64_t kHoldMs = 90;   // hold an incomplete frame this long for
                                   // retransmit/reorder before giving up
constexpr size_t kMaxFrames = 32;  // safety cap on buffered frames
const uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

bool ts_newer(uint32_t a, uint32_t b) {
    uint32_t d = a - b;
    return d != 0 && d < (1u << 31);
}

bool parse_rtp(const uint8_t* rtp, size_t len, uint16_t* seq, uint32_t* ts,
               bool* marker, const uint8_t** payload, size_t* payload_len) {
    if (len < 12) return false;
    uint8_t cc = rtp[0] & 0x0f;
    bool ext = (rtp[0] & 0x10) != 0;
    bool padding = (rtp[0] & 0x20) != 0;
    *marker = (rtp[1] & 0x80) != 0;
    *seq = static_cast<uint16_t>((rtp[2] << 8) | rtp[3]);
    *ts = (static_cast<uint32_t>(rtp[4]) << 24) | (rtp[5] << 16) |
          (rtp[6] << 8) | rtp[7];
    size_t header = 12 + 4u * cc;
    if (len < header) return false;
    if (ext) {
        if (len < header + 4) return false;
        size_t words = (rtp[header + 2] << 8) | rtp[header + 3];
        header += 4 + 4 * words;
        if (len < header) return false;
    }
    size_t plen = len - header;
    // libwebrtc sends RTP padding packets (bandwidth probing) on the video
    // stream: the P bit is set and the last byte is the padding length. Strip
    // it -- otherwise the padding bytes are misparsed as a NALU and a pure
    // padding packet (which still consumes a sequence number) looks like a lost
    // video packet, breaking frame reassembly.
    if (padding && plen > 0) {
        uint8_t pad = rtp[len - 1];
        plen = (pad <= plen) ? plen - pad : 0;
    }
    *payload = rtp + header;
    *payload_len = plen;
    return true;
}

uint8_t nal_type(const uint8_t* p, size_t len) { return len ? (p[0] & 0x1f) : 0; }

bool is_partition_head(const uint8_t* p, size_t len) {
    uint8_t t = nal_type(p, len);
    if (t == 28) return len > 1 && (p[1] & 0x80) != 0;  // FU-A start
    return t >= 1 && t <= 24;                            // single NALU or STAP-A
}

void append(std::vector<uint8_t>& au, const uint8_t* d, size_t n) {
    au.insert(au.end(), d, d + n);
}

void depacketize(const uint8_t* p, size_t len, std::vector<uint8_t>& au) {
    if (!len) return;
    uint8_t t = nal_type(p, len);
    if (t >= 1 && t <= 23) {
        append(au, kStartCode, 4);
        append(au, p, len);
    } else if (t == 24) {  // STAP-A
        size_t i = 1;
        while (i + 2 <= len) {
            size_t n = (p[i] << 8) | p[i + 1];
            i += 2;
            if (!n || i + n > len) break;
            append(au, kStartCode, 4);
            append(au, p + i, n);
            i += n;
        }
    } else if (t == 28) {  // FU-A
        if (len < 2) return;
        if (p[1] & 0x80) {  // start
            append(au, kStartCode, 4);
            au.push_back(static_cast<uint8_t>((p[0] & 0xe0) | (p[1] & 0x1f)));
        }
        append(au, p + 2, len - 2);
    }
}

bool has_keyframe(const std::vector<uint8_t>& au) {
    // IDR slices ONLY (NAL type 5). xCloud repeats SPS/PPS in-band without an
    // IDR, and accepting a bare SPS here resumed decode on P-frames whose
    // references were never decoded -- the decoder concealed them from
    // uninitialized surfaces, producing the self-sustaining green/pixelated
    // garbage. Only a real IDR rebuilds the reference chain (green-vita gates
    // on has_idr for exactly this reason).
    for (size_t i = 0; i + 4 < au.size(); ++i)
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1) {
            if ((au[i + 4] & 0x1f) == 5) return true;
        }
    return false;
}

}  // namespace

void VideoJitterBuffer::reset() {
    frames_.clear();
    last_ts_.reset();
    waiting_keyframe_ = true;
    have_seq_ = false;
    max_seq_ = 0;
    cycles_ = base_seq_ = received_ = received_prior_ = expected_prior_ = 0;
}

bool VideoJitterBuffer::report_stats(uint8_t* fraction_lost,
                                     uint32_t* cumulative_lost,
                                     uint32_t* highest_seq_ext) {
    if (!have_seq_) return false;
    uint32_t extended_max = cycles_ + max_seq_;
    uint32_t expected = extended_max - base_seq_ + 1;
    uint32_t lost = expected > received_ ? expected - received_ : 0;
    uint32_t exp_iv = expected - expected_prior_;
    uint32_t rcv_iv = received_ - received_prior_;
    expected_prior_ = expected;
    received_prior_ = received_;
    uint32_t lost_iv = exp_iv > rcv_iv ? exp_iv - rcv_iv : 0;
    *fraction_lost = (exp_iv == 0 || lost_iv == 0)
                         ? 0
                         : static_cast<uint8_t>((lost_iv << 8) / exp_iv);
    *cumulative_lost = lost & 0x00ffffff;
    *highest_seq_ext = extended_max;
    return true;
}

VideoJitterBuffer::Frame* VideoJitterBuffer::find_or_create(uint32_t ts,
                                                            uint64_t now_ms) {
    for (auto& f : frames_)
        if (f.timestamp == ts) return &f;
    // Reject frames older than what we've already emitted.
    if (last_ts_ && !ts_newer(ts, *last_ts_)) return nullptr;
    if (frames_.size() >= kMaxFrames) return nullptr;  // overflow guard
    Frame f;
    f.timestamp = ts;
    f.first_seen_ms = now_ms;
    // Insert in ascending timestamp order.
    auto it = frames_.begin();
    while (it != frames_.end() && ts_newer(ts, it->timestamp)) ++it;
    return &*frames_.insert(it, std::move(f));
}

// Assemble a complete access unit from a frame if all its packets are present
// Returns false if the frame is not yet internally complete/assemblable.
bool VideoJitterBuffer::try_assemble(Frame& f, std::vector<uint8_t>& au,
                                     uint16_t* marker_seq) {
    bool have_marker = false;
    for (const auto& pk : f.packets)
        if (pk.marker) { *marker_seq = pk.seq; have_marker = true; break; }
    if (!have_marker) return false;

    std::vector<const Packet*> ordered;
    ordered.reserve(f.packets.size());
    for (const auto& pk : f.packets) ordered.push_back(&pk);
    uint16_t ms = *marker_seq;
    std::sort(ordered.begin(), ordered.end(),
              [ms](const Packet* a, const Packet* b) {
                  return static_cast<uint16_t>(ms - a->seq) >
                         static_cast<uint16_t>(ms - b->seq);
              });

    // Emit as soon as the frame is internally complete: its lowest-seq packet
    // must be a NALU/partition head, and its packets must be seq-contiguous up
    // to the marker. We deliberately do NOT require contiguity *across* frames
    // (a strict next-seq check) -- RTP padding/probing packets sit between
    // frames and consume sequence numbers, and treating those as loss was
    // dropping every frame and freezing the stream.
    const Packet* first = ordered.front();
    if (!is_partition_head(first->payload.data(), first->payload.size()))
        return false;
    for (size_t i = 1; i < ordered.size(); ++i)
        if (ordered[i]->seq != static_cast<uint16_t>(ordered[i - 1]->seq + 1))
            return false;  // gap inside the frame -> wait for retransmit

    au.clear();
    for (const Packet* pk : ordered) {
        depacketize(pk->payload.data(), pk->payload.size(), au);
        if (au.size() > kMaxAccessUnitBytes) return false;
    }
    return true;
}

void VideoJitterBuffer::receive(const uint8_t* rtp, size_t len, uint64_t now_ms,
                                const Emit& emit, const NackFn& nack,
                                bool* want_keyframe) {
    uint16_t seq;
    uint32_t ts;
    bool marker;
    const uint8_t* payload;
    size_t payload_len;
    if (!parse_rtp(rtp, len, &seq, &ts, &marker, &payload, &payload_len)) return;
    stats_.packets++;

    // --- global sequence tracking: NACK gaps + receiver-report stats ---
    if (!have_seq_) {
        have_seq_ = true;
        base_seq_ = max_seq_ = seq;
        cycles_ = received_ = 0;
    }
    received_++;
    uint16_t udelta = static_cast<uint16_t>(seq - max_seq_);
    if (udelta != 0 && udelta < 0x8000) {  // seq is newer than max_seq_
        if (seq < max_seq_) cycles_ += 0x10000;
        uint16_t gap = static_cast<uint16_t>(udelta - 1);
        if (gap > 0 && gap <= 255) {
            uint16_t pid = static_cast<uint16_t>(max_seq_ + 1);
            while (pid != seq) {
                uint16_t blp = 0, nx = static_cast<uint16_t>(pid + 1);
                for (int b = 0; b < 16 && nx != seq; ++b, ++nx)
                    blp |= static_cast<uint16_t>(1u << b);
                nack(pid, blp);
                stats_.nacks++;
                pid = nx;
            }
        }
        max_seq_ = seq;
    }

    if (payload_len == 0) return;  // padding/empty packet: nothing to assemble

    Frame* f = find_or_create(ts, now_ms);
    if (!f) return;  // stale or overflow
    for (const auto& pk : f->packets)
        if (pk.seq == seq) return;  // duplicate/retransmit already have it
    Packet pk;
    pk.seq = seq;
    pk.marker = marker;
    pk.payload.assign(payload, payload + payload_len);
    f->packets.push_back(std::move(pk));

    // --- drain: emit complete frames in timestamp order; time out the rest ---
    std::vector<uint8_t> au;
    for (;;) {
        if (frames_.empty()) break;
        Frame& front = frames_.front();
        uint16_t marker_seq = 0;
        if (try_assemble(front, au, &marker_seq)) {
            last_ts_ = front.timestamp;
            frames_.pop_front();
            if (waiting_keyframe_) {
                if (!has_keyframe(au)) {  // still no clean IDR -> keep dropping
                    *want_keyframe = true;
                    stats_.resyncs++;
                    continue;
                }
                waiting_keyframe_ = false;
            }
            stats_.frames++;
            stats_.last_frame_bytes = static_cast<uint32_t>(au.size());
            emit(au.data(), au.size());
            continue;
        }
        // Not assemblable yet. If it has waited long enough, give up on it (a
        // packet was lost and no retransmit came) and resync from the next
        // clean keyframe -- never feed the decoder a broken reference frame.
        if (now_ms - front.first_seen_ms > kHoldMs) {
            last_ts_ = front.timestamp;
            frames_.pop_front();
            waiting_keyframe_ = true;
            *want_keyframe = true;
            stats_.dropped++;
            continue;
        }
        break;  // wait for more packets / retransmit
    }
}

}  // namespace gnx::stream
