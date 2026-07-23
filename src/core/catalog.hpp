#pragma once

#include <string>
#include <vector>

#include "auth.hpp"
#include "http.hpp"

namespace gnx {

struct Game {
    std::string title_id;    // xCloud launch id, e.g. "ASSASSINSCREEDSHADOWS"
    std::string product_id;  // Microsoft Store bigId, for metadata lookup
    std::string name;        // filled by fetch_names()
    std::string box_art_url;
};

// Titles the signed-in account can actually play (entitlement or subscription),
// from the xCloud backend's /v2/titles.
std::vector<Game> fetch_playable_titles(Http& http,
                                        const EndpointCredentials& cloud);

// Fills name/box_art_url from the public Microsoft Store display catalog.
// Batched; no authentication required.
void fetch_names(Http& http, std::vector<Game>& games,
                 const std::string& market = "US",
                 const std::string& language = "en-US");

// An Xbox console linked to the account, streamable over xHome (remote play).
struct HomeConsole {
    std::string server_id;     // target for GssvSession::start_home
    std::string name;          // user-given console name
    std::string console_type;  // e.g. "XboxSeriesX"
    std::string power_state;   // e.g. "On", "ConnectedStandby"
};

// Consoles the account can remote-play, from the xhome offering's /v6/servers/
// home. Use the xhome credentials (StreamingCredentials.home), not the cloud
// ones. Empty result = feature stays hidden in the UI.
std::vector<HomeConsole> fetch_home_consoles(
    Http& http, const EndpointCredentials& home);

}  // namespace gnx
