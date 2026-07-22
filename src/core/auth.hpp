#pragma once

#include <optional>
#include <string>

#include "http.hpp"

namespace gnx {

struct DeviceCode {
    std::string user_code;
    std::string verification_uri;
    std::string message;
    std::string device_code;
    int interval_secs = 5;
    int expires_in_secs = 900;
};

enum class PollResult { Pending, Authorized, Expired };

struct EndpointCredentials {
    std::string host;   // e.g. https://uks.gssv-play-prod.xboxlive.com
    std::string token;  // gsToken bearer
};

struct StreamingCredentials {
    EndpointCredentials home;
    EndpointCredentials cloud;
    std::optional<EndpointCredentials> cloud_f2p;
};

struct XboxProfile {
    std::string gamertag;
    std::string gamerscore;
    std::string avatar_url;
};

// Microsoft device-code OAuth + Xbox Live token chain + xCloud (GSSV) login.
// Mirrors the flow used by xbox.com/play and open clients (Greenlight, green-vita).
class XboxAuth {
public:
    explicit XboxAuth(std::string token_store_path);

    bool has_saved_login() const { return !refresh_token_.empty(); }
    void logout();

    DeviceCode request_device_code();
    PollResult poll_device_code(const DeviceCode& code);

    // Full chain: refresh -> XSTS user token -> XSTS gssv authorize ->
    // per-offering streaming logins (xhome, xgpuweb, xgpuwebf2p).
    StreamingCredentials fetch_streaming_credentials();

    XboxProfile fetch_profile();

    // Short-lived MSA token used by the session /connect handshake.
    std::string fetch_passport_token();

private:
    std::string refresh_user_token();  // returns MSA access token
    std::string xsts_user_authenticate(const std::string& access_token);
    // returns {token, uhs}
    std::pair<std::string, std::string> xsts_authorize(
        const std::string& user_token, const std::string& relying_party);
    EndpointCredentials streaming_login(const std::string& gssv_token,
                                        const std::string& offering);

    void save_refresh_token(const std::string& token);
    void load_refresh_token();

    Http http_;
    std::string store_path_;
    std::string refresh_token_;
};

}  // namespace gnx
