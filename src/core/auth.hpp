#pragma once

#include <optional>
#include <string>
#include <vector>

#include "http.hpp"

namespace gnx {

struct DeviceCode {
    std::string user_code;
    std::string verification_uri;
    // verification_uri with the code already in it, for the sign-in QR: a
    // phone that scans it lands on the code screen with nothing to type.
    std::string verification_uri_complete;
    std::string message;
    std::string device_code;
    int interval_secs = 5;
    int expires_in_secs = 900;
};

enum class PollResult { Pending, Authorized, Expired };

// One region of a streaming offering, as the login response lists it.
struct ServerRegion {
    std::string name;  // label from the login response; may be empty
    std::string host;  // base URI, e.g. https://uks.gssv-play-prod.xboxlive.com
    bool is_default = false;

    // What the settings store and the menu shows: the name when the service
    // gives one, otherwise the subdomain ("uks"), which is the region code.
    std::string label() const;
};

struct EndpointCredentials {
    std::string host;   // e.g. https://uks.gssv-play-prod.xboxlive.com
    std::string token;  // gsToken bearer
    std::vector<ServerRegion> regions;  // every region the login offered

    // A copy aimed at the region labelled `label`. An empty or unknown label
    // keeps the default host, which is what "Automatic" means.
    EndpointCredentials in_region(const std::string& label) const;
};

struct StreamingCredentials {
    EndpointCredentials home;
    // Why the xhome (remote play) login failed, if it did. An account with no
    // console attached is a normal case, so this is not an error by itself --
    // but starting a home stream without it never works, and the caller should
    // say so rather than build a request against an empty host.
    std::string home_error;
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

    // Point at a different token store and reload from it -- used to switch
    // between accounts without recreating the object (the streaming engine
    // holds a reference to it).
    void set_token_store(std::string path);

    DeviceCode request_device_code();
    PollResult poll_device_code(const DeviceCode& code);

    // When set, in-flight auth HTTP calls abort promptly once *flag becomes
    // true. Lets the sign-in worker unblock at shutdown (same contract as
    // Http::set_abort_flag).
    void set_abort_flag(std::atomic<bool>* flag) { http_.set_abort_flag(flag); }

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
