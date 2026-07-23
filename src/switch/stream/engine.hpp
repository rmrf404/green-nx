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

// Native xCloud streaming session: GSSV signaling + libpeer WebRTC +
// NVDEC/SDL video + Opus audio + gamepad input channel.
class Engine {
public:
    Engine(XboxAuth& auth, SDL_Renderer* renderer);
    ~Engine();

    void start(const std::string& title_id, QualityTier tier,
               const std::string& locale = "en-US");
    // Remote play from your own console (xhome offering): the target is the
    // console's serverId; the game is whatever the console runs.
    void start_home(const std::string& server_id, QualityTier tier,
                    const std::string& locale = "en-US");
    void stop();

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
    void run_peer(GssvSession& session);
    void set_status(const std::string& status);
    void fail(const std::string& error);
    void handle_channel_message(uint16_t sid, const char* data, size_t size);
    void handle_input_report(const uint8_t* data, size_t size);  // rumble, etc.
    void open_data_channels();
    void request_keyframe_locked();  // caller holds peer_mutex_
    void send_on_channel(const char* label, const std::string& payload);
    void send_binary_on_channel(const char* label,
                                const std::vector<uint8_t>& payload);
    // *_locked: caller already holds peer_mutex_ (callbacks run under it).
    void send_on_channel_locked(const char* label, const std::string& payload);
    void send_binary_on_channel_locked(const char* label,
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
    QualityTier tier_ = QualityTier::P1080HQ;
    std::string locale_ = "en-US";  // streamed console's system language
public:
    void log(const std::string& line);  // also used by the libpeer log sink

private:
    FILE* log_file_ = nullptr;
    std::mutex log_mutex_;

    PeerConnection* peer_ = nullptr;
    std::mutex peer_mutex_;
    std::atomic<PeerConnectionState> peer_state_{PEER_CONNECTION_NEW};
    std::atomic<bool> channels_open_{false};
    std::atomic<bool> handshake_done_{false};
    std::atomic<bool> quit_{false};

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

    // Decoded-frame handoff (Switch): decode_thread_ decodes into shared_frame_;
    // the render thread (pump_video) takes its own ref into present_frame_ so it
    // can present zero-copy while the decode thread keeps producing. Two refs of
    // the same NVTEGRA surface keep it alive across the hand-off.
    std::thread decode_thread_;
    std::mutex frame_mutex_;
    AVFrame* shared_frame_ = nullptr;   // latest decoded (decode thread writes)
    AVFrame* present_frame_ = nullptr;  // render thread's stable ref
    bool shared_frame_valid_ = false;

    xcloud::InputSerializer input_;
    std::mutex input_mutex_;

    // Server->client rumble. Written by the peer thread (handle_input_report),
    // drained by the main thread (take_rumble). Latest command wins.
    std::mutex rumble_mutex_;
    RumbleCommand rumble_cmd_;
    bool rumble_pending_ = false;
    bool rumble_logged_ = false;  // peer thread only: log the first report once
    Uint64 stream_epoch_ = 0;
    // Render-thread software vsync pacer for the deko3d present (see pump_video).
    double next_present_ms_ = 0;
    std::atomic<Uint64> last_keyframe_req_{0};
    std::atomic<uint32_t> pli_sent_{0};  // RTCP PLI keyframe requests

    std::thread thread_;
};

}  // namespace gnx::stream
