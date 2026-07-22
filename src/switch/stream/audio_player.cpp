#include "audio_player.hpp"

namespace gnx::stream {

namespace {
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kMaxFrameSamples = 5760;  // 120 ms at 48 kHz
// Cap queued audio at ~350 ms so brief hitches don't force a full-queue clear
// (which is audible as a gap); only clear when we're genuinely far behind.
constexpr Uint32 kMaxQueuedBytes =
    kSampleRate * 35 / 100 * kChannels * sizeof(int16_t);
}  // namespace

bool AudioPlayer::init() {
    int error = 0;
    decoder_ = opus_decoder_create(kSampleRate, kChannels, &error);
    if (error != OPUS_OK) return false;

    SDL_AudioSpec want{};
    want.freq = kSampleRate;
    want.format = AUDIO_S16SYS;
    want.channels = kChannels;
    want.samples = 480;  // 10 ms
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (!device_) return false;
    SDL_PauseAudioDevice(device_, 0);

    pcm_.resize(kMaxFrameSamples * kChannels);
    return true;
}

void AudioPlayer::shutdown() {
    if (device_) SDL_CloseAudioDevice(device_), device_ = 0;
    if (decoder_) opus_decoder_destroy(decoder_), decoder_ = nullptr;
}

void AudioPlayer::play(const uint8_t* data, size_t size) {
    if (!decoder_ || !device_) return;
    int samples =
        opus_decode(decoder_, data, static_cast<opus_int32>(size), pcm_.data(),
                    kMaxFrameSamples, 0);
    if (samples <= 0) {
        stats_.failed++;
        return;
    }

    if (SDL_GetQueuedAudioSize(device_) > kMaxQueuedBytes) {
        SDL_ClearQueuedAudio(device_);  // fell behind; drop to stay live
        stats_.cleared++;
    }
    SDL_QueueAudio(device_, pcm_.data(),
                   static_cast<Uint32>(samples) * kChannels * sizeof(int16_t));
    stats_.played++;
}

}  // namespace gnx::stream
