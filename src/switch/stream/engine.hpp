#pragma once

#include <SDL2/SDL.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../core/auth.hpp"
#include "../../core/session.hpp"
#include "../../core/xcloud_protocol.hpp"
#include "audio_player.hpp"
#include "video_decoder.hpp"
#include "video_jitter.hpp"
#ifdef __SWITCH__
#include "dk_video_renderer.hpp"
#endif

extern "C" {
#include <peer_connection.h>
}

namespace gnx::stream {

enum class EngineState {
    Idle,
    StartingSession,   // REST: create session, wait for provisioning
    Negotiating,       // SDP/ICE exchange + DTLS
    WaitingForVideo,   // connected, waiting for the first frame
    Streaming,
    Failed,
    Stopped,
};

// How decoded frames reach the display (Switch deko3d path).
//   Steady: present the NEWEST decoded frame on the ~60 Hz software clock --
//           lowest latency, but uneven arrival timing shows as motion hitches.
//   Smooth: keep one decoded frame in reserve and present in source order on
//           a detected 30/60 Hz cadence -- steadier motion at the cost of
//           about one source frame (~33 ms at 30 fps) of extra latency.
enum class VideoPacing { Steady = 0, Smooth = 1 };

// Native xCloud streaming session: GSSV signaling + libpeer WebRTC +
// NVDEC/SDL video + Opus audio + gamepad input channel.
class Engine {
public:
    Engine(XboxAuth& auth, SDL_Renderer* renderer);
    ~Engine();

    // Releases the process-wide WebRTC state (libsrtp, usrsctp and its two
    // service threads) that the first Engine brings up. Call once at app
    // exit, after every Engine is gone; no Engine may be created afterwards.
    static void global_shutdown();

    // ad_supported picks the xgpuwebf2p offering instead of the regular
    // cloud one: that is how a title streams without a Game Pass plan (with
    // ads, and with the server-side session cap that comes with it).
    void start(const std::string& title_id, QualityTier tier,
               const std::string& locale = "en-US",
               bool ad_supported = false);
    // Remote play from your own console (xhome offering): the target is the
    // console's serverId; the game is whatever the console runs.
    void start_home(const std::string& server_id, QualityTier tier,
                    const std::string& locale = "en-US");
    void stop();

    // Output gain applied to decoded audio (forwarded to the AudioPlayer). Set
    // from the "volume" setting before each stream start; 1.0 = unchanged.
    void set_audio_gain(float gain) { audio_gain_ = gain; }

    // Video pacing mode (see VideoPacing). Set from the "smooth" setting
    // before each stream start; default Steady.
    void set_pacing(VideoPacing pacing) { pacing_ = pacing; }

    // Luma sharpening level (0=Off..3=High), forwarded to the deko3d
    // renderer. Set from the "sharpness" setting before each stream start.
    void set_sharpness(int level) { sharpness_ = level; }

    // Draw the on-screen debug HUD overlay while streaming. Forwarded live so
    // the in-stream combo can flip it mid-session, not just before start().
    void set_debug_hud(bool enabled) {
        debug_hud_ = enabled;
#ifdef __SWITCH__
        dk_video_.set_hud_enabled(enabled);
#endif
    }

    EngineState state() const { return state_; }
    std::string status() const;
    std::string error() const;

    // Render-thread pump: decodes queued video. On Switch it presents each
    // frame through the deko3d renderer (returns nullptr); on PC it returns the
    // SDL texture (nullptr until the first frame).
    SDL_Texture* pump_video();
    int video_width() const { return video_.width(); }
    int video_height() const { return video_.height(); }

    // Switch: take over the display with deko3d for zero-copy video (call after
    // Gfx::suspend). end_deko_output releases it before Gfx::resume. On PC
    // begin returns false and end is a no-op.
    bool begin_deko_output();
    void end_deko_output();

    void send_gamepad(const xcloud::GamepadFrame& frame);
    void request_keyframe();

    // Player-triggered controller recovery (#61). The player is the only
    // reliable detector of a wedged controller: some wedges leave no trace in
    // any counter we keep. First press re-announces the pad in-band (cheap,
    // no interruption); a second press within 10 s escalates to the
    // same-session reconnect. Safe from the UI thread; the work happens on
    // the worker thread.
    void request_input_recovery() {
        manual_recovery_requests_.fetch_add(1, std::memory_order_relaxed);
    }

    // Writes one line into the active stream log. For UI-side events worth
    // seeing in a bug report (a reopened joystick handle); no-op when no
    // stream is running.
    void log_note(const std::string& line) { log(line); }

    // Controller rumble decoded from the server's "input" channel. The main
    // thread (which owns the SDL joystick) drains the latest command once per
    // frame and actuates it via SDL_JoystickRumble -- keeping every SDL joystick
    // call on one thread avoids racing SDL's own joystick bookkeeping.
    struct RumbleCommand {
        uint16_t low = 0;          // large (low-frequency) motor, 0..0xFFFF
        uint16_t high = 0;         // small (high-frequency) motor, 0..0xFFFF
        uint32_t duration_ms = 0;  // self-terminating, per the server report
    };
    bool take_rumble(RumbleCommand& out);  // true if a fresh command was pending

private:
    void start_common(const std::string& title_id, QualityTier tier,
                      const std::string& locale);
    void worker();
    void decode_loop();  // Switch: dedicated H.264 decode thread (see engine.cpp)
    // Runs the WebRTC session to completion. Returns false only when ICE
    // connected but DTLS/SCTP never came up (dead media path) -- worker()
    // then retries once with a fresh session. Every other outcome, including
    // ordinary failures, returns true.
    bool run_peer(GssvSession& session);
    // Mid-stream reconnect (#61): tears down the dead attempt's peer and
    // re-arms every piece of stream-side state that only start_common
    // normally resets, so the replacement transport negotiates against a
    // clean slate while state_ stays pinned to Streaming. Worker thread.
    void rearm_for_resume();
    // Re-announces the gamepad to the server: ClientMetadata on the input
    // channel plus a remove/add pair on control, i.e. the hotplug path the
    // server already knows how to handle. Cheap and in-band, so it is the
    // first rung of every controller recovery. Worker thread; takes
    // peer_mutex_ itself.
    void revive_input(const char* reason);
    void set_status(const std::string& status);
    void end_session();  // server closed the session: stop, not fail
    void fail(const std::string& error);
    void handle_channel_message(uint16_t sid, const char* data, size_t size);
    void handle_input_report(const uint8_t* data, size_t size);  // rumble, etc.
    void open_data_channels();
    void request_keyframe_locked();  // caller holds peer_mutex_
    void send_on_channel(const char* label, const std::string& payload);
    // *_locked: caller already holds peer_mutex_ (callbacks run under it).
    void send_on_channel_locked(const char* label, const std::string& payload);
    // Returns false when the channel is missing or SCTP rejected the payload
    // (send buffer full during an outage) -- send_gamepad rolls the input
    // sequence number back on failure to keep the numbering contiguous.
    bool send_binary_on_channel_locked(const char* label,
                                       const std::vector<uint8_t>& payload);

    static void on_video(uint8_t* data, size_t size, void* user);
    static void on_audio(uint8_t* data, size_t size, void* user);
    static void on_channel_message(char* data, size_t size, void* user,
                                   uint16_t sid);
    static void on_channel_open(void* user);
    static void on_state_change(PeerConnectionState state, void* user);

    XboxAuth& auth_;
    SDL_Renderer* renderer_;
    Http http_;  // worker-thread HTTP client

    std::atomic<EngineState> state_{EngineState::Idle};
    mutable std::mutex status_mutex_;
    std::string status_;
    std::string error_;

    EndpointCredentials cloud_;
    std::string title_id_;
    std::string home_server_id_;  // non-empty selects the home (xhome) path
    bool ad_supported_ = false;   // selects the xgpuwebf2p offering
    QualityTier tier_ = QualityTier::P1080HQ;
    std::string locale_ = "en-US";  // streamed console's system language
    float audio_gain_ = 1.0f;       // forwarded to AudioPlayer::set_gain
    VideoPacing pacing_ = VideoPacing::Steady;  // set before start()
    int sharpness_ = 0;  // 0=Off..3=High, forwarded to DkVideoRenderer
public:
    void log(const std::string& line);  // also used by the libpeer log sink

private:
    FILE* log_file_ = nullptr;
    std::mutex log_mutex_;

    PeerConnection* peer_ = nullptr;
    // timed_mutex: the render/input thread takes it with a bounded wait
    // (send_gamepad) so a wedged worker can never freeze presentation and
    // input polling behind it (#45).
    std::timed_mutex peer_mutex_;
    std::atomic<PeerConnectionState> peer_state_{PEER_CONNECTION_NEW};
    std::atomic<bool> channels_open_{false};
    std::atomic<bool> handshake_done_{false};
    std::atomic<bool> quit_{false};
    // Set when the server sends serverInitiatedDisconnect (stream stopped on
    // the console, console powering off, another client took over).
    std::atomic<bool> server_ended_{false};
    // run_peer -> worker: the datachannel died mid-stream (#61), retry with
    // a fresh session on the reconnect budget instead of the dead-path one.
    std::atomic<bool> reconnect_requested_{false};
    // True while a mid-stream reconnect is replacing the session. state_ must
    // stay Streaming for its whole duration: the moment it leaves, the main
    // loop's deko hand-off quiesces the engine (stop(), the #33 guard) and
    // aborts the very session request the reconnect depends on. Cleared when
    // the replacement session decodes its first frame.
    std::atomic<bool> resuming_{false};
    // Bumped by rearm_for_resume() under video_mutex_. An AU popped from the
    // queue carries the generation it was popped under; a decode that
    // finishes with a stale generation belongs to the dead stream and must
    // not publish a frame or set got_frame_ -- doing so would clear resuming_
    // early, unpin state_, and let the main loop quiesce the engine in the
    // middle of the reconnect.
    std::atomic<uint32_t> stream_gen_{0};
    // Last RTP arrival, video or audio (peer thread). run_peer's stall
    // watchdog uses it to end a stream whose media path died silently.
    std::atomic<Uint64> last_media_ticks_{0};
    // Heartbeat of run_peer's pump loop, stored every iteration. The
    // keepalive thread watches it: the in-loop watchdogs can't see the loop
    // itself wedging inside libpeer (#45), an outside thread can.
    std::atomic<Uint64> worker_tick_{0};
    // Input-path telemetry, logged once per second (input| line): frames
    // sent, dropped waiting for peer_mutex_, rejected by SCTP. The
    // dead-controller reports in #45 were undebuggable without this.
    std::atomic<uint32_t> input_sent_{0};
    std::atomic<uint32_t> input_drop_lock_{0};
    std::atomic<uint32_t> input_send_fail_{0};
    // Server -> client reports on the "input" channel (vibration etc.). The
    // server accepting our packets while its own input side goes silent is
    // the only client-visible trace of the server-side input wedge (#61,
    // second kind) -- sends succeed, so the failure counters above are blind.
    std::atomic<uint32_t> input_rx_{0};
    std::atomic<Uint64> input_rx_last_{0};  // tick of the newest such report
    // Send-buffer backpressure. A refused input send means usrsctp's send
    // buffer is full (EAGAIN): the association stopped draining. Hammering it
    // 125 times a second only adds pressure and holds peer_mutex_ away from
    // the media pump, so back off briefly instead.
    std::atomic<Uint64> input_backoff_until_{0};
    std::atomic<uint32_t> input_backoff_skips_{0};
    // request_input_recovery() -> worker thread, and the tick of the last
    // manual attempt so a second press can escalate.
    std::atomic<uint32_t> manual_recovery_requests_{0};
    std::atomic<Uint64> last_manual_recovery_{0};

    VideoDecoder video_;  // width()/height() are render-thread reads only
#ifdef __SWITCH__
    DkVideoRenderer dk_video_;  // render-thread: zero-copy NVTEGRA -> display
#endif
    VideoJitterBuffer jitter_;  // worker-thread only (RTP -> access units)
    AudioPlayer audio_;
    std::mutex video_mutex_;
    std::condition_variable video_cv_;  // wakes decode_loop when an AU arrives
    std::deque<std::vector<uint8_t>> video_queue_;
    std::atomic<bool> got_frame_{false};
    std::atomic<uint64_t> video_bytes_{0};  // RTP video bytes rx (HUD bitrate)

    // Decoded-frame handoff (Switch): decode_thread_ decodes into shared_frame_;
    // the render thread (pump_video) takes its own ref into present_frame_ so it
    // can present zero-copy while the decode thread keeps producing. Two refs of
    // the same NVTEGRA surface keep it alive across the hand-off.
    std::thread decode_thread_;
    std::mutex frame_mutex_;
    AVFrame* shared_frame_ = nullptr;   // latest decoded (decode thread writes)
    AVFrame* present_frame_ = nullptr;  // render thread's stable ref
    bool shared_frame_valid_ = false;
    uint64_t shared_frame_seq_ = 0;     // protected by frame_mutex_

    // Smooth pacing (VideoPacing::Smooth): decoded frames queue in source
    // order instead of newest-wins; pump_video presents them on a detected
    // 30/60 Hz cadence with one frame held in reserve to absorb arrival
    // jitter. The queue is capped hard: each entry pins an NVTEGRA surface
    // from the decoder's small pool, so letting it grow would starve NVDEC.
    struct SmoothFrame {
        AVFrame* frame = nullptr;
        uint64_t seq = 0;
    };
    std::deque<SmoothFrame> smooth_frames_;  // protected by frame_mutex_
    bool smooth_have_present_ = false;       // render thread only
    uint32_t smooth_refresh_phase_ = 0;      // render thread only
    std::atomic<uint32_t> source_refresh_period_{1};  // 1=60fps, 2=30fps
    uint32_t source_fast_streak_ = 0;  // decode thread only
    uint32_t source_slow_streak_ = 0;  // decode thread only
    // Written by the decode thread on every decoded frame; also read by the
    // worker's video-stall watchdog (and reset by it after a suspension).
    std::atomic<Uint64> last_decode_ticks_{0};

    // Pacing telemetry, logged once per second from run_peer (pace| line):
    // new/repeated presents, how many refreshes each frame stayed up, and
    // frames skipped (newest-wins jumps or smooth-queue overflow drops).
    uint64_t last_present_seq_ = 0;        // render thread only
    uint32_t present_hold_refreshes_ = 0;  // render thread only
    std::atomic<uint32_t> pace_new_{0}, pace_repeat_{0};
    std::atomic<uint32_t> pace_hold1_{0}, pace_hold2_{0};
    std::atomic<uint32_t> pace_hold3_{0}, pace_hold4p_{0};
    std::atomic<uint32_t> pace_skip_{0};

    xcloud::InputSerializer input_;
    std::mutex input_mutex_;

    // Server->client rumble. Written by the peer thread (handle_input_report),
    // drained by the main thread (take_rumble). Latest command wins.
    std::mutex rumble_mutex_;
    RumbleCommand rumble_cmd_;
    bool rumble_pending_ = false;
    bool rumble_logged_ = false;  // peer thread only: log the first report once
    Uint64 stream_epoch_ = 0;
    // Render-thread software vsync pacer for the deko3d present (see
    // pump_video), in SDL performance-counter ticks: millisecond deadlines
    // quantized to an uneven 16/17 ms grid; the counter keeps the fraction.
    double next_present_counter_ = 0;
    bool debug_hud_ = false;                // draw the debug HUD overlay
    std::atomic<Uint64> last_keyframe_req_{0};
    std::atomic<uint32_t> pli_sent_{0};  // RTCP PLI keyframe requests

    std::thread thread_;
};

}  // namespace gnx::stream
