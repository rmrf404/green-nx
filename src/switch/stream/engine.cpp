#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <sstream>
#include <cstring>

extern "C" {
#include <peer.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
}

extern "C" void gnx_peer_log_set(void (*cb)(const char* line));

#ifndef GNX_VERSION
#define GNX_VERSION "dev"
#endif

namespace {
// libpeer's LOG_REDIRECT sink funnels through this single active engine.
gnx::stream::Engine* g_log_engine = nullptr;

// Route ffmpeg's own diagnostics (H.264 reference errors, concealment, ...)
// into stream-log; without this the decoder's complaints are invisible.
void av_log_capture(void* avcl, int level, const char* fmt, va_list vl) {
    (void)avcl;
    if (level > AV_LOG_WARNING) return;
    static std::atomic<int> lines{0};
    if (lines.fetch_add(1) >= 300) return;  // never flood the SD card
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    size_t n = std::strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    if (n && g_log_engine)
        g_log_engine->log(std::string("ffmpeg| ") + buf);
}

void install_av_log_capture() { av_log_set_callback(&av_log_capture); }
}

namespace gnx::stream {

namespace {
// Safety cap only: each queue entry is one H.264 NALU, and pump_video drains
// the whole queue every render frame, so this is normally near-empty. Dropping
// individual NALUs corrupts the stream, so on overflow we clear and recover
// with a keyframe instead.
constexpr size_t kMaxQueuedVideo = 64;

struct TierProfile {
    int width, height, bitrate_kbps, fps;
};

TierProfile tier_profile(QualityTier tier) {
    switch (tier) {
        case QualityTier::P720: return {1280, 720, 10000, 60};
        case QualityTier::P1080: return {1920, 1080, 20000, 60};
        case QualityTier::P1080HQ: return {1920, 1080, 30000, 60};
    }
    return {1920, 1080, 20000, 60};
}

// Extract "candidate:..." lines from a local SDP for the /ice POST.
std::vector<std::string> local_candidates_from_sdp(const std::string& sdp) {
    std::vector<std::string> out;
    size_t at = 0;
    while ((at = sdp.find("a=candidate:", at)) != std::string::npos) {
        size_t end = sdp.find_first_of("\r\n", at);
        out.push_back(sdp.substr(at + 2, end - at - 2));
        at = end == std::string::npos ? sdp.size() : end;
    }
    return out;
}

std::string ufrag_from_sdp(const std::string& sdp) {
    size_t at = sdp.find("a=ice-ufrag:");
    if (at == std::string::npos) return "";
    at += std::strlen("a=ice-ufrag:");
    size_t end = sdp.find_first_of("\r\n", at);
    return sdp.substr(at, end - at);
}

}  // namespace

Engine::Engine(XboxAuth& auth, SDL_Renderer* renderer)
    : auth_(auth), renderer_(renderer) {
    http_.set_abort_flag(&quit_);  // don't block shutdown on an HTTP call
    // One-time global init of libsrtp + usrsctp. Without this, srtp_create()
    // fails (no inbound SRTP -> no decryptable video) and usrsctp never
    // associates (data channels never open). Idempotent guard: Engine is a
    // singleton, but be safe.
    static bool peer_initialized = false;
    if (!peer_initialized) {
        peer_init();
        peer_initialized = true;
    }
}

Engine::~Engine() { stop(); }

void Engine::log(const std::string& line) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (!log_file_) return;
    std::fprintf(log_file_, "[%8llu] %s\n",
                 static_cast<unsigned long long>(SDL_GetTicks64()),
                 line.c_str());
    std::fflush(log_file_);
}

void Engine::start(const std::string& title_id, QualityTier tier,
                   const std::string& locale) {
    stop();
    title_id_ = title_id;
    tier_ = tier;
    locale_ = locale;
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (log_file_) std::fclose(log_file_);
#ifdef __SWITCH__
        // Keep the previous session's log: rotate instead of overwrite.
        std::remove("sdmc:/switch/green-nx/stream-log-prev.txt");
        std::rename("sdmc:/switch/green-nx/stream-log.txt",
                    "sdmc:/switch/green-nx/stream-log-prev.txt");
        log_file_ = std::fopen("sdmc:/switch/green-nx/stream-log.txt", "w");
#else
        log_file_ = stderr;
#endif
    }
    g_log_engine = this;
    gnx_peer_log_set([](const char* line) {
        if (g_log_engine) g_log_engine->log(std::string("  peer| ") + line);
    });
    const char* tier_name = tier == QualityTier::P720      ? "720p/android"
                            : tier == QualityTier::P1080   ? "1080p/windows"
                                                           : "1080pHQ/tizen";
    log("green-nx v" GNX_VERSION " | stream start: " + title_id + " | tier " +
        tier_name);
    quit_ = false;
    got_frame_ = false;
    channels_open_ = false;
    handshake_done_ = false;
    pli_sent_ = 0;
    install_av_log_capture();
    jitter_.reset();
    next_present_ms_ = 0;  // first frame presents immediately, then paced
    state_ = EngineState::StartingSession;
    video_.init(renderer_);
    audio_.init();
#ifdef __SWITCH__
    shared_frame_ = av_frame_alloc();
    present_frame_ = av_frame_alloc();
    shared_frame_valid_ = false;
#endif
    stream_epoch_ = SDL_GetTicks64();
    thread_ = std::thread(&Engine::worker, this);
#ifdef __SWITCH__
    // Decode runs on its own thread so hardware-decode latency never delays
    // input polling or the vsync-paced present on the main thread.
    decode_thread_ = std::thread(&Engine::decode_loop, this);
#endif
}

void Engine::stop() {
    quit_ = true;
    video_cv_.notify_all();  // wake the decode thread so it can see quit_
    if (thread_.joinable()) thread_.join();
    if (decode_thread_.joinable()) decode_thread_.join();
    if (g_log_engine == this) {
        gnx_peer_log_set(nullptr);
        g_log_engine = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (log_file_ && log_file_ != stderr) std::fclose(log_file_);
        log_file_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        if (peer_) {
            peer_connection_close(peer_);
            peer_connection_destroy(peer_);
            peer_ = nullptr;
        }
    }
    video_.shutdown();
    audio_.shutdown();
    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        video_queue_.clear();
    }
#ifdef __SWITCH__
    {
        // Decode thread is joined; safe to release the hand-off frames (unrefs
        // any held NVTEGRA surface back to the decoder's pool).
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (shared_frame_) av_frame_free(&shared_frame_);
        if (present_frame_) av_frame_free(&present_frame_);
        shared_frame_valid_ = false;
    }
#endif
    if (state_ != EngineState::Failed) state_ = EngineState::Stopped;
}

std::string Engine::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

std::string Engine::error() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return error_;
}

void Engine::set_status(const std::string& status) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_ = status;
}

void Engine::fail(const std::string& error) {
    log("FAIL: " + error);
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        error_ = error;
    }
    state_ = EngineState::Failed;
}

// ---- libpeer callbacks ----------------------------------------------------

void Engine::on_video(uint8_t* data, size_t size, void* user) {
    // Called on the worker thread inside peer_connection_loop() (peer_mutex_
    // held). `data` is a raw RTP packet; the jitter buffer reorders/assembles
    // complete access units and only emits clean, keyframe-anchored frames.
    auto* self = static_cast<Engine*>(user);
    bool want_keyframe = false;
    self->jitter_.receive(
        data, size, SDL_GetTicks64(),
        [self](const uint8_t* au, size_t au_size) {
            {
                std::lock_guard<std::mutex> lock(self->video_mutex_);
                if (self->video_queue_.size() >= kMaxQueuedVideo)
                    self->video_queue_.clear();
                self->video_queue_.emplace_back(au, au + au_size);
            }
            self->video_cv_.notify_one();  // wake the decode thread (Switch)
        },
        [self](uint16_t pid, uint16_t blp) {
            // Retransmit request for lost packets (peer_mutex_ already held).
            if (self->peer_) peer_connection_send_nack(self->peer_, pid, blp);
        },
        &want_keyframe);
    if (want_keyframe) self->request_keyframe_locked();
}

void Engine::on_audio(uint8_t* data, size_t size, void* user) {
    // Called on the worker thread with peer_mutex_ held. `data` is a whole RTP
    // packet (rtp_decode_generic forwards header+payload, like the H.264 path).
    // Parse the header to find the Opus payload and the sequence number, then
    // hand it straight to the audio thread -- decode happens there, not here, so
    // audio never waits behind video/RTCP work on this thread.
    auto* self = static_cast<Engine*>(user);
    if (size < 12) return;
    uint8_t csrc_count = data[0] & 0x0F;
    bool has_extension = (data[0] & 0x10) != 0;
    bool has_padding = (data[0] & 0x20) != 0;
    uint16_t seq = (static_cast<uint16_t>(data[2]) << 8) | data[3];

    size_t offset = 12 + static_cast<size_t>(csrc_count) * 4;
    if (has_extension) {
        if (offset + 4 > size) return;
        uint16_t ext_words =
            (static_cast<uint16_t>(data[offset + 2]) << 8) | data[offset + 3];
        offset += 4 + static_cast<size_t>(ext_words) * 4;
    }
    size_t end = size;
    if (has_padding && end > offset) {
        uint8_t pad = data[end - 1];
        if (pad <= end - offset) end -= pad;
    }
    if (offset > end) return;
    self->audio_.submit(seq, data + offset, end - offset);
}

void Engine::on_channel_message(char* data, size_t size, void* user,
                                uint16_t sid) {
    static_cast<Engine*>(user)->handle_channel_message(sid, data, size);
}

void Engine::on_channel_open(void* user) {
    static_cast<Engine*>(user)->channels_open_ = true;
}

void Engine::on_state_change(PeerConnectionState state, void* user) {
    static_cast<Engine*>(user)->peer_state_ = state;
}

// ---- data channel plumbing ------------------------------------------------

void Engine::open_data_channels() {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    if (!peer_) return;
    // The DTLS client uses even SCTP stream ids (RFC 8832). xCloud maps each
    // channel by its DCEP label, so the exact ids only need to be distinct.
    struct { const xcloud::ChannelConfig& cfg; uint16_t sid; } channels[] = {
        {xcloud::kControlChannel, 0},
        {xcloud::kInputChannel, 2},
        {xcloud::kMessageChannel, 4},
        {xcloud::kChatChannel, 6},
    };
    for (const auto& channel : channels) {
        DecpChannelType type =
            channel.cfg.max_retransmits == 0
                ? (channel.cfg.ordered ? DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT
                                       : DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED)
                : (channel.cfg.ordered ? DATA_CHANNEL_RELIABLE
                                       : DATA_CHANNEL_RELIABLE_UNORDERED);
        uint32_t reliability = channel.cfg.max_retransmits < 0
                                   ? 0
                                   : static_cast<uint32_t>(channel.cfg.max_retransmits);
        peer_connection_create_datachannel_sid(
            peer_, type, 0, reliability, const_cast<char*>(channel.cfg.label),
            const_cast<char*>(channel.cfg.protocol), channel.sid);
    }
    log("opened data channels (control/input/message/chat)");
}

void Engine::send_on_channel(const char* label, const std::string& payload) {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    send_on_channel_locked(label, payload);
}

// Caller must hold peer_mutex_. Used from callbacks that libpeer already
// invokes with the lock held (see handle_channel_message).
void Engine::send_on_channel_locked(const char* label,
                                    const std::string& payload) {
    if (!peer_) return;
    uint16_t sid = 0;
    // control/message/chat carry JSON -> must be WebRTC string frames, or
    // xCloud ignores them (handshake + clientdevicecapabilities/quality).
    if (peer_connection_lookup_sid(peer_, label, &sid) == 0) {
        peer_connection_datachannel_send_text_sid(
            peer_, const_cast<char*>(payload.data()), payload.size(), sid);
        log("send [" + std::string(label) + " sid=" + std::to_string(sid) +
            "] " + payload.substr(0, 220));
    } else {
        log("send FAILED (no channel '" + std::string(label) + "')");
    }
}

void Engine::send_binary_on_channel(const char* label,
                                    const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    send_binary_on_channel_locked(label, payload);
}

void Engine::send_binary_on_channel_locked(const char* label,
                                           const std::vector<uint8_t>& payload) {
    if (!peer_) return;
    uint16_t sid = 0;
    if (peer_connection_lookup_sid(peer_, label, &sid) == 0)
        peer_connection_datachannel_send_sid(
            peer_,
            const_cast<char*>(reinterpret_cast<const char*>(payload.data())),
            payload.size(), sid);
}

void Engine::handle_channel_message(uint16_t sid, const char* data,
                                    size_t size) {
    // IMPORTANT: libpeer invokes this from inside peer_connection_loop(), which
    // the worker already runs while holding peer_mutex_. peer_mutex_ is not
    // recursive, so we must NOT re-lock it here (doing so froze the worker the
    // instant xCloud's first message arrived -> stuck on "Handshaking"). peer_
    // is guaranteed alive for the duration of this callback.
    char* label = peer_ ? peer_connection_lookup_sid_label(peer_, sid) : nullptr;
    if (label && std::strcmp(label, "input") == 0) {
        // Binary telemetry/rumble from the server. Handle it here and return so
        // the raw bytes don't spam the log -- vibration reports can arrive many
        // times a second while a game is rumbling.
        handle_input_report(reinterpret_cast<const uint8_t*>(data), size);
        return;
    }
    // Log every inbound control/message payload so the exact xCloud protocol
    // exchange is visible in stream-log.txt during bring-up.
    {
        std::string preview(data, std::min<size_t>(size, 220));
        log("recv [" + std::string(label ? label : "sid?") + " sid=" +
            std::to_string(sid) + " len=" + std::to_string(size) + "] " +
            preview);
    }
    if (!label) return;

    if (std::strcmp(label, "message") == 0 && !handshake_done_) {
        if (xcloud::is_handshake_ack(std::string(data, size))) {
            // Handshake acked: authorize the control channel, announce the
            // gamepad, then declare client capabilities (our quality lever).
            send_on_channel_locked("control", xcloud::authorization_request());
            send_on_channel_locked("control", xcloud::gamepad_changed(0, true));
            TierProfile profile = tier_profile(tier_);
            for (const std::string& message : xcloud::startup_messages(
                     profile.width, profile.height, profile.bitrate_kbps,
                     profile.fps))
                send_on_channel_locked("message", message);
            {
                std::lock_guard<std::mutex> lock(input_mutex_);
                send_binary_on_channel_locked("input", input_.client_metadata());
            }
            // Ask for an IDR immediately (both the RTCP PLI that xCloud
            // actually acts on, and the app-level message) so video can start
            // instead of waiting for the server's periodic keyframe.
            peer_connection_request_keyframe(peer_);
            send_on_channel_locked("control", xcloud::video_keyframe_requested());
            last_keyframe_req_ = SDL_GetTicks64();
            log("handshake complete, capabilities sent");
            handshake_done_ = true;
            if (state_ == EngineState::Negotiating)
                state_ = EngineState::WaitingForVideo;
        }
    }
}

void Engine::handle_input_report(const uint8_t* data, size_t size) {
    // Server "input"-channel report. We only act on Vibration (type 128). The
    // wire layout matches the xbox.com/play client (ref: greenlight):
    //   [0]  report type (128 = Vibration)
    //   [2]  rumble type (0 = four-motor)     [3]  gamepad index
    //   [4]  left motor %   [5]  right motor %
    //   [6]  left-trigger % [7]  right-trigger %   (all 0..100)
    //   [8:2] duration ms (LE)  [10:2] delay ms (LE)  [12] repeat count
    if (size < 13 || data[0] != 128) return;

    auto pct = [](uint8_t v) { return v >= 100 ? 1.0f : v / 100.0f; };
    // The Switch has no trigger actuators. Fold the trigger motors into the LOW
    // band (a duller thud) instead of the high band: driving the high band hard
    // produces an audible, harsh whine, and shooters hammer the triggers.
    float low_pct = pct(data[4]) + (pct(data[6]) + pct(data[7])) * 0.5f;
    if (low_pct > 1.0f) low_pct = 1.0f;
    float high_pct = pct(data[5]);

    uint16_t duration = static_cast<uint16_t>(data[8] | (data[9] << 8));
    uint16_t delay = static_cast<uint16_t>(data[10] | (data[11] << 8));
    uint8_t repeat = data[12];

    // Each report is self-terminating: SDL plays the effect for duration_ms and
    // stops on its own, exactly like the browser client's fixed-duration effect
    // -- so no "stop" packet is needed (the input channel is unreliable). For
    // repeated pulses we approximate the whole envelope as one window (the
    // off-gaps can't be reproduced through SDL) and cap it, so a corrupt length
    // can never leave a motor stuck on.
    uint32_t duration_ms = duration;
    if (repeat > 0)
        duration_ms += static_cast<uint32_t>(repeat) * (duration + delay);
    if (duration_ms > 4000) duration_ms = 4000;

    RumbleCommand cmd;
    cmd.low = static_cast<uint16_t>(low_pct * 65535.0f);
    cmd.high = static_cast<uint16_t>(high_pct * 65535.0f);
    cmd.duration_ms = duration_ms;
    {
        std::lock_guard<std::mutex> lock(rumble_mutex_);
        rumble_cmd_ = cmd;
        rumble_pending_ = true;
    }
    if (!rumble_logged_) {
        rumble_logged_ = true;
        log("rumble: first server vibration report received");
    }
}

bool Engine::take_rumble(RumbleCommand& out) {
    std::lock_guard<std::mutex> lock(rumble_mutex_);
    if (!rumble_pending_) return false;
    out = rumble_cmd_;
    rumble_pending_ = false;
    return true;
}

// ---- worker ---------------------------------------------------------------

void Engine::worker() {
    try {
        set_status("Signing in to xCloud...");
        cloud_ = auth_.fetch_streaming_credentials().cloud;

        set_status("Cleaning up old sessions...");
        GssvSession::cleanup_stale_sessions(http_, cloud_);

        set_status("Requesting a session...");
        GssvSession session(http_, cloud_, tier_, locale_);
        session.start_cloud(title_id_);

        set_status("Waiting for a server...");
        bool connected = false;
        for (int i = 0; i < 300 && !quit_; ++i) {
            SessionState state = session.refresh_state();
            if (state == SessionState::ReadyToConnect && !connected) {
                set_status("Authenticating...");
                session.connect(auth_.fetch_passport_token());
                connected = true;
            } else if (state == SessionState::Provisioned) {
                run_peer(session);
                session.stop();
                return;
            } else if (state == SessionState::Failed) {
                fail("Session failed: " + session.error_details());
                session.stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
        }
        session.stop();
        if (!quit_) fail("Timed out waiting for a session");
    } catch (const std::exception& error) {
        fail(error.what());
    }
}

void Engine::run_peer(GssvSession& session) {
    state_ = EngineState::Negotiating;
    set_status("Negotiating connection...");

    PeerConfiguration config{};
    config.ice_servers[0].urls = "stun:stun.l.google.com:19302";
    config.audio_codec = CODEC_OPUS;
    config.video_codec = CODEC_H264;
    config.datachannel = DATA_CHANNEL_BINARY;
    config.onvideotrack = &Engine::on_video;
    config.onaudiotrack = &Engine::on_audio;
    config.user_data = this;

    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        peer_ = peer_connection_create(&config);
        if (!peer_) {
            fail("Failed to create peer connection");
            return;
        }
        peer_connection_oniceconnectionstatechange(peer_,
                                                   &Engine::on_state_change);
        // NOTE: the client must open these channels, but libpeer can only send
        // the DATA_CHANNEL_OPEN once the SCTP association is up. We therefore
        // defer creation until on_channel_open (SCTP connected) fires -- see
        // open_data_channels() in the negotiation loop below.
        peer_connection_ondatachannel(peer_, &Engine::on_channel_message,
                                      &Engine::on_channel_open, nullptr);
    }

    const char* offer = nullptr;
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        offer = peer_connection_create_offer(peer_);
    }
    if (!offer) {
        fail("Failed to create SDP offer");
        return;
    }
    log("local offer created (" + std::to_string(std::strlen(offer)) +
        " bytes)");

    // The base offer now matches the known-good native client's template
    // exactly (recvonly, PT 102, full fmtp, goog-remb/fir, stereo opus).
    // No b=AS/TIAS lines: working clients don't send them; the bitrate cap is
    // declared via clientdevicecapabilities.maxBitrateKbps instead.
    std::string munged = sdp_force_stereo(offer);  // no-op safety net
    // 720p tier ships the template verbatim (proven accepted); 1080p tiers
    // scale the declared decode capability to 1080p60.
    if (tier_ != QualityTier::P720)
        munged = sdp_scale_video_caps_1080(munged);
    // Pass the answer to libpeer VERBATIM. Never rewrite it: the server has
    // already chosen the codec, and any reserialization risks corrupting the
    // CRLF line endings, which would make libpeer parse the ICE ufrag/pwd with
    // a stray '\r' and send STUN checks with a wrong integrity key (silently
    // dropped by the server -> connection never completes).
    std::string answer = session.exchange_sdp(munged);
    log("answer received (" + std::to_string(answer.size()) + " bytes)");
    // Dump both SDPs for offline inspection of ICE/setup/codec lines.
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (log_file_) {
            std::fprintf(log_file_, "----- OFFER -----\n%s\n----- ANSWER -----\n%s\n-----\n",
                         munged.c_str(), answer.c_str());
            std::fflush(log_file_);
        }
    }

    // Our candidates go to the server over /ice (they are already embedded
    // in the offer SDP too, but the official client posts them explicitly).
    try {
        std::vector<std::string> local = local_candidates_from_sdp(munged);
        std::string ufrag = ufrag_from_sdp(munged);
        log("posting " + std::to_string(local.size()) +
            " local candidates (ufrag " + ufrag + ")");
        if (!local.empty()) session.send_ice_candidates(local, ufrag);
    } catch (const std::exception& error) {
        log(std::string("local candidate post failed: ") + error.what());
    }

    // IMPORTANT: xCloud trickles its candidates via /ice, not in the answer
    // SDP — and libpeer builds candidate pairs exactly once, inside
    // set_remote_description. So collect the server's candidates FIRST.
    std::vector<std::string> remote;
    {
        Uint64 gather_deadline = SDL_GetTicks64() + 15000;
        bool done = false;
        int quiet_polls = 0;
        // xCloud first returns a placeholder front candidate (priority 100 on
        // 13.104.x) that never answers STUN; the REAL (Teredo) candidate can
        // trickle in seconds later. Settling for the placeholder alone makes
        // ICE fail, so keep polling until a real candidate shows up.
        auto has_real_candidate = [&remote]() {
            for (const std::string& c : remote) {
                // candidate:<found> <comp> UDP <priority> ...
                size_t sp = 0;
                int field = 0;
                unsigned long prio = 0;
                std::istringstream ss(c);
                std::string tok;
                while (ss >> tok && field < 4) {
                    if (field == 3) prio = std::strtoul(tok.c_str(), nullptr, 10);
                    field++;
                }
                (void)sp;
                if (prio > 1000) return true;
            }
            return false;
        };
        while (!quit_ && !done && SDL_GetTicks64() < gather_deadline) {
            size_t before = remote.size();
            try {
                for (std::string& candidate :
                     session.receive_ice_candidates(&done))
                    remote.push_back(std::move(candidate));
            } catch (const std::exception& error) {
                log(std::string("ice poll failed: ") + error.what());
            }
            quiet_polls = remote.size() == before ? quiet_polls + 1 : 0;
            // No end marker but candidates stopped coming: assume complete --
            // but never settle while all we have is the dead placeholder.
            if (!remote.empty() && has_real_candidate() && quiet_polls >= 4)
                break;
            if (!done)
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }
    log("collected " + std::to_string(remote.size()) + " remote candidates");
    for (const std::string& candidate : remote) log("  remote cand: " + candidate);
    for (const std::string& candidate : local_candidates_from_sdp(munged))
        log("  local  cand: " + candidate);
    if (remote.empty()) {
        fail("Server sent no ICE candidates");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        for (const std::string& candidate : remote)
            peer_connection_add_ice_candidate(
                peer_, const_cast<char*>(candidate.c_str()));
        // Builds pairs from every remote candidate above, then -> CHECKING.
        peer_connection_set_remote_description(peer_, answer.c_str(),
                                               SDP_TYPE_ANSWER);
    }
    log("remote description set, checking connectivity");

    Uint64 last_keepalive = SDL_GetTicks64();
    Uint64 last_rr = SDL_GetTicks64();
    Uint64 last_consent = SDL_GetTicks64();
    Uint64 last_audio_stats = SDL_GetTicks64();
    Uint64 prev_audio_time = SDL_GetTicks64();
    uint32_t prev_audio_frames = 0;
    uint32_t prev_audio_out = 0;
    Uint64 idr_wait_start = 0;
    Uint64 last_idr_wait_log = 0;
    Uint64 negotiation_started = SDL_GetTicks64();
    bool opened_channels = false;
    bool sent_handshake = false;
    PeerConnectionState last_logged_state = PEER_CONNECTION_NEW;

    while (!quit_) {
        // Drain all packets ready on the socket this cycle. Video at 1080p is
        // ~2500 packets/s; processing one-per-iteration-then-sleeping dropped
        // most of them (socket-buffer overflow) and wrecked the video. We drain
        // in bounded batches so the render thread can still grab peer_mutex_ to
        // send input between batches, and only sleep when the socket is idle.
        bool drained_any = false;
        {
            std::lock_guard<std::mutex> lock(peer_mutex_);
            for (int i = 0; peer_ && i < 64; ++i) {
                if (peer_connection_loop(peer_) > 0)
                    drained_any = true;
                else
                    break;  // socket empty (select timed out) -> stop draining
            }
        }

        Uint64 now = SDL_GetTicks64();
        PeerConnectionState current = peer_state_;
        if (current != last_logged_state) {
            last_logged_state = current;
            log(std::string("peer state: ") +
                peer_connection_state_to_string(current));
        }

        // channels_open_ is set from libpeer's SCTP onopen (association up).
        // Only now can DATA_CHANNEL_OPEN be sent; open our channels with
        // distinct even (DTLS-client) stream ids, then start the handshake.
        if (channels_open_ && !opened_channels) {
            opened_channels = true;
            open_data_channels();
            set_status("Handshaking...");
        }

        if (opened_channels && !sent_handshake) {
            sent_handshake = true;
            send_on_channel("message", xcloud::message_handshake());
        }

        if (peer_state_ == PEER_CONNECTION_FAILED) {
            fail("WebRTC connection failed");
            return;
        }
        if (state_ == EngineState::Negotiating &&
            SDL_GetTicks64() - negotiation_started > 45000) {
            fail("Connection timed out");
            return;
        }

        // Until the first frame decodes, keep asking for a keyframe. xCloud may
        // start mid-GOP (only P-frames) or drop our first request; a single
        // request isn't enough. request_keyframe_locked() self-throttles to 1/s.
        if (handshake_done_ && !got_frame_) {
            std::lock_guard<std::mutex> lock(peer_mutex_);
            request_keyframe_locked();
        }

        // Make an IDR drought visible: if the jitter buffer keeps waiting for a
        // real keyframe, say so (with how long and how many PLIs went out)
        // instead of silently dropping frames.
        if (handshake_done_ && jitter_.waiting_keyframe()) {
            if (!idr_wait_start) idr_wait_start = now;
            if (now - last_idr_wait_log >= 2000 && now - idr_wait_start >= 2000) {
                last_idr_wait_log = now;
                log("waiting for IDR (" +
                    std::to_string((now - idr_wait_start) / 1000) + "s, " +
                    std::to_string(pli_sent_.load()) + " PLIs sent)");
            }
        } else {
            idr_wait_start = 0;
        }

        // Periodic RTCP Receiver Report + REMB: standard receiver etiquette
        // (loss accounting + bandwidth headroom signal).
        if (now - last_rr > 1000) {
            last_rr = now;
            uint8_t fraction;
            uint32_t cumulative, highest_ext;
            if (jitter_.report_stats(&fraction, &cumulative, &highest_ext)) {
                std::lock_guard<std::mutex> lock(peer_mutex_);
                if (peer_) {
                    peer_connection_send_receiver_report(peer_, fraction,
                                                         cumulative, highest_ext, 0);
                    peer_connection_send_remb(
                        peer_,
                        static_cast<uint32_t>(tier_profile(tier_).bitrate_kbps) *
                            1000u);
                }
            }
        }

        // Audio pipeline telemetry: cumulative counters logged once per second
        // so a dropout shows up as its cause (loss vs. queue starvation vs.
        // decode failure) instead of a guess. Only meaningful once streaming.
        if (now - last_audio_stats > 1000) {
            auto a = audio_.stats();
            uint32_t in_hz = (now > prev_audio_time)
                ? (a.frames - prev_audio_frames) * 1000 / (now - prev_audio_time)
                : 0;
            uint32_t out_hz = (now > prev_audio_time)
                ? (a.out_samples - prev_audio_out) * 1000 / (now - prev_audio_time)
                : 0;
            prev_audio_frames = a.frames;
            prev_audio_out = a.out_samples;
            prev_audio_time = now;
            last_audio_stats = now;
            if (got_frame_) {
                log("audio| rx=" + std::to_string(a.received) +
                    " play=" + std::to_string(a.played) +
                    " fail=" + std::to_string(a.failed) +
                    " lost=" + std::to_string(a.lost) +
                    " under=" + std::to_string(a.underruns) +
                    " drop=" + std::to_string(a.dropped_ms) + "ms" +
                    " q=" + std::to_string(a.queue_ms) + "ms" +
                    " in=" + std::to_string(in_hz) + "hz" +
                    " out=" + std::to_string(out_hz) + "hz" +
                    " dev=" + std::to_string(audio_.device_hz()) + "hz" +
                    " ema=" + std::to_string(a.ema_ms) + "ms" +
                    " adj=" + std::to_string(a.adj_ppm) + "ppm");
            }
        }

        // ICE consent freshness (RFC 7675): keep the peer's consent to send us
        // media alive. A full WebRTC stack does this every ~5s; libpeer doesn't.
        if (now - last_consent > 2000) {
            last_consent = now;
            std::lock_guard<std::mutex> lock(peer_mutex_);
            if (peer_) peer_connection_send_consent(peer_);
        }

        if (now - last_keepalive > 15000) {
            last_keepalive = now;
            try {
                session.keepalive();
            } catch (const std::exception&) {}
        }

        // Only yield when idle. While video is flowing we loop right back and
        // keep draining at full speed (the select() inside paces idle cycles).
        if (!drained_any)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ---- render-thread interface ----------------------------------------------

#ifdef __SWITCH__
// Dedicated decode thread. Pops assembled access units, hardware-decodes each
// (NVTEGRA), and hands the freshest decoded surface to the render thread through
// shared_frame_. Decoding here rather than inline in pump_video keeps the main
// thread free for input and a steady vsync-paced present. Every AU is decoded in
// order (P-frames reference earlier frames); the render thread just presents
// whichever frame is latest, so intermediate frames are dropped at present time,
// never skipped at decode time.
void Engine::decode_loop() {
    while (!quit_) {
        std::vector<uint8_t> unit;
        {
            std::unique_lock<std::mutex> lock(video_mutex_);
            video_cv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                return quit_.load() || !video_queue_.empty();
            });
            if (quit_) break;
            if (video_queue_.empty()) continue;
            unit = std::move(video_queue_.front());
            video_queue_.pop_front();
        }
        if (video_.decode(unit.data(), unit.size())) {
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                av_frame_unref(shared_frame_);
                av_frame_ref(shared_frame_, video_.current_frame());
                shared_frame_valid_ = true;
            }
            if (!got_frame_) {
                got_frame_ = true;
                state_ = EngineState::Streaming;
            }
        }
        // Recover from packet loss / corrupt frames with a fresh keyframe
        // (throttled) instead of staying blocky until the next periodic IDR.
        if (video_.take_error()) request_keyframe();
    }
}
#endif

SDL_Texture* Engine::pump_video() {
#ifdef __SWITCH__
    // Present-only: decode_thread_ produces frames. Present the freshest decoded
    // frame on a STEADY software clock (~59.9 Hz), not once per network frame:
    //  * Stutter: presenting on network arrival ties the flip cadence to arrival
    //    jitter, which drifts against the panel's 60 Hz -> periodic judder even
    //    on a fast link. A steady local clock decouples the two.
    //  * Green screen: re-presenting the held frame when nothing new decoded
    //    keeps a static / low-fps scene (e.g. a "syncing save" screen where
    //    xCloud nearly stops sending) from decaying to an empty surface -- which
    //    the YUV->RGB shader turns bright green.
    // The rate is a hair UNDER 60 Hz on purpose: deko3d aborts (acquireImage ->
    // DkResult_Fail) if we queue frames faster than the compositor drains them.
    // We take our OWN ref of the shared frame so the decode thread can keep
    // producing without recycling the surface the GPU is still sampling.
    constexpr double kPresentIntervalMs = 1000.0 / 59.9;  // ~16.69 ms
    double now = static_cast<double>(SDL_GetTicks64());
    if (dk_video_.initialized() && got_frame_ && now >= next_present_ms_) {
        AVFrame* frame = nullptr;
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (shared_frame_valid_) {
                av_frame_unref(present_frame_);
                if (av_frame_ref(present_frame_, shared_frame_) == 0)
                    frame = present_frame_;
            }
        }
        if (frame) dk_video_.render(frame);
        next_present_ms_ += kPresentIntervalMs;
        if (next_present_ms_ < now) next_present_ms_ = now + kPresentIntervalMs;
    }
    return nullptr;
#else
    // PC: no decode thread (SDL texture upload must stay on this render thread),
    // so decode inline and hand back the SDL texture.
    for (;;) {
        std::vector<uint8_t> unit;
        {
            std::lock_guard<std::mutex> lock(video_mutex_);
            if (video_queue_.empty()) break;
            unit = std::move(video_queue_.front());
            video_queue_.pop_front();
        }
        if (video_.decode(unit.data(), unit.size()) && !got_frame_) {
            got_frame_ = true;
            state_ = EngineState::Streaming;
        }
    }
    if (video_.take_error()) request_keyframe();
    return got_frame_ ? video_.texture() : nullptr;
#endif
}

bool Engine::begin_deko_output() {
#ifdef __SWITCH__
    dk_video_.set_logger([this](const char* m) { log(std::string(m)); });
    bool ok = dk_video_.init();
    log(ok ? "deko3d output started" : "deko3d output FAILED to start");
    return ok;
#else
    return false;
#endif
}

void Engine::end_deko_output() {
#ifdef __SWITCH__
    dk_video_.shutdown();
    log("deko3d output stopped");
#endif
}

void Engine::send_gamepad(const xcloud::GamepadFrame& frame) {
    if (!handshake_done_) return;
    std::vector<uint8_t> packet;
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        packet = input_.gamepad_packet(
            frame, static_cast<double>(SDL_GetTicks64() - stream_epoch_));
    }
    send_binary_on_channel("input", packet);
}

void Engine::request_keyframe() {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    request_keyframe_locked();
}

// Caller must hold peer_mutex_ (used from on_video, which runs under it).
void Engine::request_keyframe_locked() {
    if (!handshake_done_ || !peer_) return;
    // Throttle: at most one request per second.
    Uint64 now = SDL_GetTicks64();
    if (now - last_keyframe_req_.load() < 1000) return;
    last_keyframe_req_ = now;
    pli_sent_++;
    peer_connection_request_keyframe(peer_);  // RTCP PLI (the one xCloud honors)
    send_on_channel_locked("control", xcloud::video_keyframe_requested());
}

}  // namespace gnx::stream
