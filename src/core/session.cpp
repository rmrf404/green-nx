#include "session.hpp"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../../vendor/json.hpp"

using nlohmann::json;

namespace gnx {

namespace {

const char* os_name(QualityTier tier) {
    switch (tier) {
        case QualityTier::P720: return "android";
        case QualityTier::P1080: return "windows";
        case QualityTier::P1080HQ: return "tizen";
    }
    return "windows";
}

// x-ms-device-info fingerprint matching the requested tier.
std::string device_info_header(QualityTier tier) {
    json info = {
        {"appInfo",
         {{"env",
           {{"clientAppId", "www.xbox.com"},
            {"clientAppType", "browser"},
            {"clientAppVersion", "26.1.97"},
            {"clientSdkVersion", "10.3.7"},
            {"httpEnvironment", "prod"},
            {"sdkInstallId", ""}}}}},
        {"dev",
         {{"hw", {{"make", "Microsoft"}, {"model", "unknown"}, {"sdktype", "web"}}},
          {"os", {{"name", os_name(tier)}, {"ver", "22631.2715"}, {"platform", "desktop"}}},
          {"displayInfo",
           // Report the tier's real resolution like working clients do
           // (green-vita: 1280x720). A fabricated 4096x2160 risks landing in
           // an unknown-device profile server-side.
           {{"dimensions",
             {{"widthInPixels", tier == QualityTier::P720 ? 1280 : 1920},
              {"heightInPixels", tier == QualityTier::P720 ? 720 : 1080}}},
            {"pixelDensity", {{"dpiX", 1}, {"dpiY", 1}}}}},
          {"browser",
           {{"browserName", "chrome"}, {"browserVersion", "140.0.3485.54"}}}}},
    };
    return "X-MS-Device-Info: " + info.dump();
}

json parse_or_throw(const HttpResponse& response, const char* label) {
    if (!response.ok())
        throw std::runtime_error(std::string(label) + " failed with HTTP " +
                                 std::to_string(response.status) + ": " +
                                 response.body.substr(0, 400));
    if (response.body.empty()) return json{{"status", response.status}};
    json parsed = json::parse(response.body, nullptr, false);
    if (parsed.is_discarded())
        throw std::runtime_error(std::string(label) + ": invalid JSON");
    return parsed;
}

void throw_on_exchange_error(const json& value, const char* label) {
    if (value.contains("errorDetails") && !value["errorDetails"].is_null())
        throw std::runtime_error(std::string(label) + ": " +
                                 value["errorDetails"].dump());
}

// A Teredo IPv6 address (RFC 4380) embeds the node's public IPv4 address and
// UDP port, both bit-inverted. xCloud advertises its real media endpoint this
// way; decode it to a plain IPv4 host we can actually reach. Mirrors
// greenlight's teredo.ts. Returns false if `addr` isn't a full Teredo address.
bool decode_teredo(const std::string& addr, std::string* ipv4, int* port) {
    std::vector<std::string> groups;
    size_t start = 0;
    while (true) {
        size_t colon = addr.find(':', start);
        groups.push_back(addr.substr(start, colon - start));
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    if (groups.size() != 8) return false;         // needs the expanded form
    if (groups[0] != "2001") return false;         // Teredo prefix 2001:0000

    auto hex = [](const std::string& value) {
        return static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 16));
    };
    unsigned port_field = hex(groups[5]);
    std::string ip_hex = groups[6] + groups[7];
    if (ip_hex.size() != 8) return false;

    unsigned bytes[4];
    for (int i = 0; i < 4; ++i)
        bytes[i] = (~hex(ip_hex.substr(i * 2, 2))) & 0xFF;
    *port = static_cast<int>((~port_field) & 0xFFFF);
    *ipv4 = std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." +
            std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
    return true;
}

}  // namespace

GssvSession::GssvSession(Http& http, EndpointCredentials credentials,
                         QualityTier tier, std::string locale)
    : http_(http),
      credentials_(std::move(credentials)),
      tier_(tier),
      locale_(std::move(locale)) {}

std::string GssvSession::url(const std::string& suffix) const {
    std::string path = session_path_;
    if (!path.empty() && path.front() != '/') path = "/" + path;
    return credentials_.host + path + suffix;
}

std::vector<std::string> GssvSession::headers() const {
    return {
        "Accept: application/json",
        "Content-Type: application/json",
        "X-Gssv-Client: XboxComBrowser",
        device_info_header(tier_),
        "Authorization: Bearer " + credentials_.token,
    };
}

void GssvSession::start_cloud(const std::string& title_id) {
    json body = {
        {"clientSessionId", ""},
        {"titleId", title_id},
        {"systemUpdateGroup", ""},
        {"settings",
         {{"nanoVersion", "V3;WebrtcTransport.dll"},
          {"enableOptionalDataCollection", false},
          {"enableTextToSpeech", false},
          {"highContrast", 0},
          {"locale", locale_},
          {"useIceConnection", false},
          {"timezoneOffsetMinutes", 120},
          {"sdkType", "web"},
          {"osName", os_name(tier_)}}},
        {"serverId", ""},
        {"fallbackRegionNames", json::array()},
    };
    json response = parse_or_throw(
        http_.post(credentials_.host + "/v5/sessions/cloud/play", body.dump(),
                   headers()),
        "session start");
    session_path_ = response.at("sessionPath");
    state_ = SessionState::New;
}

SessionState GssvSession::refresh_state() {
    json response = parse_or_throw(http_.get(url("/state"), headers()),
                                   "session state");
    std::string state = response.value("state", "");
    if (state == "Provisioning") state_ = SessionState::Provisioning;
    else if (state == "WaitingForResources")
        state_ = SessionState::WaitingForResources;
    else if (state == "ReadyToConnect") state_ = SessionState::ReadyToConnect;
    else if (state == "Provisioned") state_ = SessionState::Provisioned;
    else if (state == "Failed" || state == "Error") {
        state_ = SessionState::Failed;
        error_details_ = response.value("errorDetails", json::object()).dump();
    }
    return state_;
}

void GssvSession::connect(const std::string& passport_token) {
    json body = {{"userToken", passport_token}};
    parse_or_throw(http_.post(url("/connect"), body.dump(), headers()),
                   "session connect");
}

std::string GssvSession::exchange_sdp(const std::string& offer_sdp) {
    json body = {
        {"messageType", "offer"},
        {"sdp", offer_sdp},
        {"requestId", "1"},
        {"configuration",
         {{"chatConfiguration",
           {{"bytesPerSample", 2},
            {"expectedClipDurationMs", 20},
            {"format", {{"codec", "opus"}, {"container", "webm"}}},
            {"numChannels", 1},
            {"sampleFrequencyHz", 24000}}},
          {"chat", {{"minVersion", 1}, {"maxVersion", 1}}},
          {"control", {{"minVersion", 1}, {"maxVersion", 3}}},
          {"input", {{"minVersion", 1}, {"maxVersion", 9}}},
          {"message", {{"minVersion", 1}, {"maxVersion", 1}}},
          {"reliableinput", {{"minVersion", 9}, {"maxVersion", 9}}},
          {"unreliableinput", {{"minVersion", 9}, {"maxVersion", 9}}}}},
    };
    parse_or_throw(http_.post(url("/sdp"), body.dump(), headers()),
                   "sdp offer");

    for (int attempt = 0; attempt < 120; ++attempt) {
        json response =
            parse_or_throw(http_.get(url("/sdp"), headers()), "sdp poll");
        if (response.value("status", 0) == 204) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        throw_on_exchange_error(response, "sdp exchange");
        std::string exchange = response.value("exchangeResponse", "");
        json parsed = json::parse(exchange, nullptr, false);
        if (parsed.is_discarded() || !parsed.contains("sdp"))
            throw std::runtime_error("sdp answer missing sdp payload");
        return parsed["sdp"];
    }
    throw std::runtime_error("timed out waiting for SDP answer");
}

void GssvSession::send_ice_candidates(
    const std::vector<std::string>& candidates, const std::string& ufrag) {
    // Match the official web client / greenlight exactly: messageType
    // "iceCandidate", key "candidate" (singular) holding an array of
    // stringified candidate objects, each carrying our ice-ufrag. Getting
    // this shape wrong makes xCloud withhold its real (Teredo) candidate and
    // ignore our connectivity checks.
    json list = json::array();
    for (const std::string& candidate : candidates) {
        json entry = {{"candidate", candidate},
                      {"sdpMid", "0"},
                      {"sdpMLineIndex", 0},
                      {"usernameFragment", ufrag}};
        list.push_back(entry.dump());
    }
    // Terminate the list, as the web client does.
    list.push_back(json{{"candidate", "a=end-of-candidates"},
                        {"sdpMid", "0"},
                        {"sdpMLineIndex", 0},
                        {"usernameFragment", ufrag}}
                       .dump());
    json body = {{"messageType", "iceCandidate"}, {"candidate", list}};
    json response = parse_or_throw(
        http_.post(url("/ice"), body.dump(), headers()), "ice send");
    throw_on_exchange_error(response, "ice send");
}

std::vector<std::string> GssvSession::receive_ice_candidates(
    bool* end_of_candidates) {
    if (end_of_candidates) *end_of_candidates = false;
    json response =
        parse_or_throw(http_.get(url("/ice"), headers()), "ice poll");
    if (response.value("status", 0) == 204) return {};
    throw_on_exchange_error(response, "ice poll");

    std::string exchange = response.value("exchangeResponse", "");
    json parsed = json::parse(exchange, nullptr, false);
    if (parsed.is_discarded()) return {};

    const json* items = nullptr;
    if (parsed.is_array()) items = &parsed;
    else if (parsed.contains("candidates")) items = &parsed["candidates"];
    if (!items) return {};

    std::vector<std::string> out;
    int synthetic_foundation = 20;
    for (const json& item : *items) {
        json entry = item;
        if (entry.is_string())
            entry = json::parse(entry.get<std::string>(), nullptr, false);
        if (entry.is_discarded() || !entry.is_object()) continue;
        std::string candidate = entry.value("candidate", "");
        if (candidate.rfind("a=", 0) == 0) candidate = candidate.substr(2);
        if (candidate.find("end-of-candidates") != std::string::npos) {
            if (end_of_candidates) *end_of_candidates = true;
            continue;
        }
        if (candidate.rfind("candidate:", 0) != 0) continue;
        while (!candidate.empty() &&
               (candidate.back() == ' ' || candidate.back() == '\r'))
            candidate.pop_back();

        // Tokens: candidate:<foundation> <comp> <proto> <prio> <addr> <port> ...
        std::istringstream fields(candidate);
        std::vector<std::string> parts;
        std::string token;
        while (fields >> token) parts.push_back(token);
        if (parts.size() < 6) continue;
        const std::string& address = parts[4];

        if (address.find(':') == std::string::npos) {
            out.push_back(candidate);  // plain IPv4
            continue;
        }
        // IPv6: only usable if it's a Teredo address encoding an IPv4 endpoint.
        std::string ipv4;
        int teredo_port = 0;
        if (!decode_teredo(address, &ipv4, &teredo_port)) continue;
        // xCloud reaches this node on the decoded port and on UDP 9002.
        for (int port : {teredo_port, 9002}) {
            out.push_back("candidate:" + std::to_string(synthetic_foundation++) +
                          " 1 UDP 1 " + ipv4 + " " + std::to_string(port) +
                          " typ host");
        }
    }
    return out;
}

void GssvSession::keepalive() {
    parse_or_throw(http_.post(url("/keepalive"), "", headers()), "keepalive");
}

void GssvSession::stop() {
    if (session_path_.empty()) return;
    try {
        http_.del(url(""), headers());
    } catch (const std::exception&) {}
}

void GssvSession::cleanup_stale_sessions(
    Http& http, const EndpointCredentials& credentials) {
    GssvSession probe(http, credentials);
    try {
        json response = parse_or_throw(
            http.get(credentials.host + "/v5/sessions/cloud/active",
                     probe.headers()),
            "active sessions");
        std::vector<std::string> paths;
        // Collect any "sessionPath"/"path" strings anywhere in the response.
        std::function<void(const json&)> walk = [&](const json& node) {
            if (node.is_array())
                for (const json& item : node) walk(item);
            else if (node.is_object())
                for (auto& [key, value] : node.items()) {
                    if ((key == "sessionPath" || key == "path") &&
                        value.is_string())
                        paths.push_back(value);
                    walk(value);
                }
        };
        walk(response);
        for (const std::string& path : paths) {
            std::string full = path;
            if (!full.empty() && full.front() != '/') full = "/" + full;
            try {
                http.del(credentials.host + full, probe.headers());
            } catch (const std::exception&) {}
        }
    } catch (const std::exception&) {
        // 404 = no active-session endpoint; nothing to clean.
    }
}

// ---- SDP munging ----------------------------------------------------------

std::string sdp_force_stereo(const std::string& sdp) {
    const std::string needle = "useinbandfec=1";
    std::string result = sdp;
    size_t at = result.find(needle);
    if (at != std::string::npos &&
        result.find("stereo=1") == std::string::npos)
        result.insert(at + needle.size(), ";stereo=1");
    return result;
}

std::string sdp_scale_video_caps_1080(const std::string& sdp) {
    // The base offer declares 720p decode capability (max-fs=3600 MBs,
    // max-mbps=108000). For 1080p tiers scale to 1920x1080@60: 8160 MBs,
    // 489600 MB/s. Same-length in-place replacements keep the SDP intact.
    std::string out = sdp;
    size_t at = out.find("max-fs=3600");
    if (at != std::string::npos) out.replace(at + 7, 4, "8160");
    at = out.find("max-mbps=108000");
    if (at != std::string::npos) out.replace(at + 9, 6, "489600");
    return out;
}

}  // namespace gnx
