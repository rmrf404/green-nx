#include "xcloud_protocol.hpp"

#include <cstring>

#include "../../vendor/json.hpp"

using nlohmann::json;

namespace gnx::xcloud {

namespace {

constexpr uint16_t kReportGamepad = 2;
constexpr uint16_t kReportClientMetadata = 8;

template <typename T>
void put(std::vector<uint8_t>& out, T value) {
    uint8_t raw[sizeof(T)];
    std::memcpy(raw, &value, sizeof(T));  // little-endian hosts only
    out.insert(out.end(), raw, raw + sizeof(T));
}

int16_t axis(float value) {
    float scaled = value * 32767.0f;
    if (scaled > 32767.0f) scaled = 32767.0f;
    if (scaled < -32767.0f) scaled = -32767.0f;
    return static_cast<int16_t>(scaled);
}

uint16_t trigger(float value) {
    if (value < 0) return 0;
    float scaled = value * 65535.0f;
    if (scaled > 65535.0f) scaled = 65535.0f;
    return static_cast<uint16_t>(scaled);
}

}  // namespace

std::vector<uint8_t> InputSerializer::client_metadata(
    uint8_t max_touchpoints) {
    std::vector<uint8_t> out;
    out.reserve(15);
    put<uint16_t>(out, kReportClientMetadata);
    put<uint32_t>(out, ++sequence_);
    put<double>(out, 0.0);
    out.push_back(max_touchpoints);
    return out;
}

std::vector<uint8_t> InputSerializer::gamepad_packet(const GamepadFrame& pad,
                                                     double elapsed_ms) {
    std::vector<uint8_t> out;
    out.reserve(14 + 1 + 23);
    put<uint16_t>(out, kReportGamepad);
    put<uint32_t>(out, ++sequence_);
    put<double>(out, elapsed_ms);

    out.push_back(1);  // one gamepad frame
    out.push_back(pad.index);

    uint16_t mask = 0;
    auto bit = [&](bool pressed, uint16_t value) {
        if (pressed) mask |= value;
    };
    bit(pad.nexus, 2);
    bit(pad.menu, 4);
    bit(pad.view, 8);
    bit(pad.a, 16);
    bit(pad.b, 32);
    bit(pad.x, 64);
    bit(pad.y, 128);
    bit(pad.dpad_up, 256);
    bit(pad.dpad_down, 512);
    bit(pad.dpad_left, 1024);
    bit(pad.dpad_right, 2048);
    bit(pad.left_shoulder, 4096);
    bit(pad.right_shoulder, 8192);
    bit(pad.left_thumb, 16384);
    bit(pad.right_thumb, 32768);
    put<uint16_t>(out, mask);

    put<int16_t>(out, axis(pad.left_x));
    put<int16_t>(out, axis(-pad.left_y));
    put<int16_t>(out, axis(pad.right_x));
    put<int16_t>(out, axis(-pad.right_y));
    put<uint16_t>(out, trigger(pad.left_trigger));
    put<uint16_t>(out, trigger(pad.right_trigger));
    put<uint32_t>(out, 1);  // physical unit id (LE)
    // one extra constant, big-endian in the reference client
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(1);
    return out;
}


std::string authorization_request() {
    return json{{"message", "authorizationRequest"},
                {"accessKey", "4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E"}}
        .dump();
}

std::string gamepad_changed(uint8_t index, bool was_added) {
    return json{{"message", "gamepadChanged"},
                {"gamepadIndex", index},
                {"wasAdded", was_added}}
        .dump();
}

std::string video_keyframe_requested() {
    return json{{"message", "videoKeyframeRequested"}, {"ifrRequested", true}}
        .dump();
}

std::string message_handshake() {
    return json{{"type", "Handshake"},
                {"version", "messageV1"},
                {"id", "be0bfc6d-1e83-4c8a-90ed-fa8601c5a179"},
                {"cv", "0"}}
        .dump();
}

bool is_handshake_ack(const std::string& payload) {
    json parsed = json::parse(payload, nullptr, false);
    return !parsed.is_discarded() &&
           parsed.value("type", "") == "HandshakeAck";
}

namespace {

std::string wrap_message(const char* path, const json& content, int counter) {
    return json{{"type", "Message"},
                {"content", content.dump()},
                {"id", "5c5f2b40-0000-4000-8000-00000000" +
                           std::to_string(1000 + counter)},
                {"target", path},
                {"cv", ""}}
        .dump();
}

}  // namespace

std::vector<std::string> startup_messages(int width, int height,
                                          int max_bitrate_kbps, int fps) {
    int counter = 0;
    std::vector<std::string> out;
    out.push_back(wrap_message(
        "/streaming/systemUi/configuration",
        json{{"version", {0, 2, 0}}, {"systemUis", json::array()}}, counter++));
    out.push_back(wrap_message(
        "/streaming/properties/clientappinstallidchanged",
        json{{"clientAppInstallId", "c97d7ee0-73b2-4239-bf1d-9d805a338429"}},
        counter++));
    out.push_back(wrap_message("/streaming/characteristics/orientationchanged",
                               json{{"orientation", 0}}, counter++));
    out.push_back(
        wrap_message("/streaming/characteristics/touchinputenabledchanged",
                     json{{"touchInputEnabled", false}}, counter++));
    out.push_back(wrap_message(
        "/streaming/characteristics/clientdevicecapabilities",
        json{{"supportsCustomResolution", true},
             {"supportsHevc", false},
             {"supportsHdr", false},
             {"supportsFps", fps},
             {"maxWidth", width},
             {"maxHeight", height},
             {"maxBitrateKbps", max_bitrate_kbps},
             {"video",
              {{"width", width},
               {"height", height},
               {"maxWidth", width},
               {"maxHeight", height},
               {"maxBitrateKbps", max_bitrate_kbps}}}},
        counter++));
    out.push_back(wrap_message(
        "/streaming/characteristics/dimensionschanged",
        json{{"horizontal", width},
             {"vertical", height},
             {"preferredWidth", width},
             {"preferredHeight", height},
             {"safeAreaLeft", 0},
             {"safeAreaTop", 0},
             {"safeAreaRight", width},
             {"safeAreaBottom", height},
             {"supportsCustomResolution", true}},
        counter++));
    return out;
}

}  // namespace gnx::xcloud
