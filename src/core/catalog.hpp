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

}  // namespace gnx
