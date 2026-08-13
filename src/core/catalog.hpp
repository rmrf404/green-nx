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

    // How this account can reach the title. Several can be true at once: a
    // game you own is usually in the subscription catalog as well.
    bool owned = false;            // hasEntitlement — your own copy
    bool subscription = false;     // offered under some Game Pass tier
    bool in_subscription = false;  // ... a tier this account actually holds
    bool ad_supported = false;     // F2P program — no subscription needed
    bool ad_granted = false;       // userPrograms holds F2P for this title
    bool ad_playable = false;      // the xgpuwebf2p offering will stream it
    int max_session_secs = 0;      // ad-supported runs are capped; 0 = no cap

    // What the library grid lists.
    bool playable() const { return owned || in_subscription || ad_playable; }

    // Playable, but only through the ad-supported offering — the session has
    // to be created against StreamingCredentials::cloud_f2p rather than the
    // regular cloud one. Owning the game (or having it in your plan) wins:
    // that path carries no ads and no session cap.
    bool needs_ad_offering() const {
        return ad_playable && !owned && !in_subscription;
    }
};

// Every title the xCloud backend knows about (~2400), each tagged with how —
// or whether — this account can play it, from /v2/titles. Sorted by title id,
// duplicates removed. Callers that only want the library show the ones where
// Game::playable() holds; the rest exist so search can answer "is this game
// on xCloud at all, and what would I need to play it?".
std::vector<Game> fetch_catalog(Http& http, const EndpointCredentials& cloud);

// Folds the ad-supported offering's catalog into the cloud one. Both come
// from fetch_catalog(), each with its own offering credentials: the flags in
// `ads` describe what xgpuwebf2p will stream to this account, which is the
// only way to know a title is playable without a subscription. Titles the
// offering carries but the cloud catalog does not are appended, so a
// Game Pass-less account still gets a library instead of an empty grid.
void merge_ad_supported(std::vector<Game>& catalog,
                        const std::vector<Game>& ads);

// Casefolded, punctuation-free form of a title or query. Title ids are the
// squashed product name (FORZAHORIZON5, ASSASSINSCREEDSHADOWS), so comparing
// on this key lets "forza horizon" match a title whose name has not been
// resolved yet — i.e. search covers the whole catalog with no network at all.
std::string search_key(const std::string& text);

// True when the query key appears in the game's name or title id. An empty
// query matches everything.
bool matches_query(const Game& game, const std::string& query_key);

// Fills name/box_art_url from the public Microsoft Store display catalog.
// Batched; no authentication required. Entries that already carry a name are
// skipped, so resolving a search result set repeatedly stays cheap.
//
// Resolving the whole catalog this way is not viable: the Details template
// weighs ~53 KB per product, i.e. ~130 MB for every title. Resolve the
// playable list plus whatever the user is actually looking at.
void fetch_names(Http& http, const std::vector<Game*>& games,
                 const std::string& market = "US",
                 const std::string& language = "en-US");
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
