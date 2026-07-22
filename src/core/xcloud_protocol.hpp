#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gnx::xcloud {

// Application-layer protocol on top of the WebRTC data channels
// ("input" binary packets + "control"/"message" JSON), as spoken by
// xbox.com/play. Reference: greenlight, green-vita.

// ---- data channel configs (label / protocol / reliability) ----------------
struct ChannelConfig {
    const char* label;
    const char* protocol;
    bool ordered;
    int max_retransmits;  // -1 = reliable
};

constexpr ChannelConfig kControlChannel{"control", "controlV1", true, -1};
constexpr ChannelConfig kInputChannel{"input", "1.0", false, 0};
constexpr ChannelConfig kMessageChannel{"message", "messageV1", true, -1};
constexpr ChannelConfig kChatChannel{"chat", "chatV1", true, -1};

// ---- gamepad state --------------------------------------------------------
struct GamepadFrame {
    uint8_t index = 0;
    bool nexus = false, menu = false, view = false;
    bool a = false, b = false, x = false, y = false;
    bool dpad_up = false, dpad_down = false, dpad_left = false,
         dpad_right = false;
    bool left_shoulder = false, right_shoulder = false;
    bool left_thumb = false, right_thumb = false;
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;  // -1..1
    float left_trigger = 0, right_trigger = 0;               // 0..1
};

// Serializes gamepad reports for the "input" channel with a running sequence.
class InputSerializer {
public:
    // First packet after the channel opens.
    std::vector<uint8_t> client_metadata(uint8_t max_touchpoints = 0);
    std::vector<uint8_t> gamepad_packet(const GamepadFrame& frame,
                                        double elapsed_ms);

private:
    uint32_t sequence_ = 0;
};

// ---- control / message channel JSON ---------------------------------------
std::string authorization_request();
std::string gamepad_changed(uint8_t index, bool was_added);
std::string video_keyframe_requested();
std::string message_handshake();
bool is_handshake_ack(const std::string& payload);

// Sent on the "message" channel after HandshakeAck. Declares client
// capabilities incl. resolution and max bitrate — this is our quality lever.
std::vector<std::string> startup_messages(int width, int height,
                                          int max_bitrate_kbps, int fps);

}  // namespace gnx::xcloud
