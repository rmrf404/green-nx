#include "audio_jitter.hpp"

#include <utility>

namespace gnx::stream {

namespace {
// Give up waiting for a missing packet once this many later ones have piled up
// behind it (matches green-vita's AUDIO_MAX_LATE_PACKETS). At one 20 ms Opus
// frame per packet that is far more slack than any normal reordering needs.
constexpr size_t kMaxLate = 32;
}  // namespace

void AudioJitterBuffer::reset() {
    pending_.clear();
    next_seq_ = 0;
    have_next_ = false;
    lost_ = 0;
}

void AudioJitterBuffer::push(uint16_t seq, const uint8_t* data, size_t size) {
    if (!have_next_) {
        next_seq_ = seq;
        have_next_ = true;
    }
    // Already delivered past this sequence -- arrived too late, drop it.
    if (seq_before(seq, next_seq_)) return;
    // Duplicate of something already buffered, drop it.
    for (const auto& e : pending_)
        if (e.seq == seq) return;
    pending_.push_back({seq, std::vector<uint8_t>(data, data + size)});
}

bool AudioJitterBuffer::pop(std::vector<uint8_t>& out) {
    if (pending_.empty()) return false;

    auto find_seq = [&](uint16_t want) -> int {
        for (size_t i = 0; i < pending_.size(); ++i)
            if (pending_[i].seq == want) return static_cast<int>(i);
        return -1;
    };

    int idx = find_seq(next_seq_);
    if (idx < 0) {
        // The next packet hasn't arrived yet. Wait for it, unless too many
        // later packets have piled up -- then treat the gap as lost and skip
        // ahead to the oldest packet we're still holding.
        if (pending_.size() < kMaxLate) return false;
        uint16_t oldest = pending_.front().seq;
        for (const auto& e : pending_)
            if (seq_before(e.seq, oldest)) oldest = e.seq;
        lost_ += static_cast<uint16_t>(oldest - next_seq_);
        next_seq_ = oldest;
        idx = find_seq(next_seq_);
        if (idx < 0) return false;  // defensive; should not happen
    }

    out = std::move(pending_[idx].data);
    pending_.erase(pending_.begin() + idx);
    ++next_seq_;
    return true;
}

}  // namespace gnx::stream
