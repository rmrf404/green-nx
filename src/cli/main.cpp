// PC test harness for the green-nx core: sign in and list playable games
// from the terminal, before any Switch code exists.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "../core/auth.hpp"
#include "../core/catalog.hpp"
#include "../core/session.hpp"

namespace {

std::string token_store_path() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.green-nx-tokens.json";
}

int cmd_login(gnx::XboxAuth& auth) {
    gnx::DeviceCode code = auth.request_device_code();
    std::printf("\nTo sign in, open %s\nand enter code: %s\n\nWaiting",
                code.verification_uri.c_str(), code.user_code.c_str());
    std::fflush(stdout);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(code.expires_in_secs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(code.interval_secs));
        switch (auth.poll_device_code(code)) {
            case gnx::PollResult::Authorized:
                std::printf("\nSigned in!\n");
                return 0;
            case gnx::PollResult::Expired:
                std::printf("\nCode expired, run login again.\n");
                return 1;
            case gnx::PollResult::Pending:
                std::printf(".");
                std::fflush(stdout);
                break;
        }
    }
    std::printf("\nTimed out.\n");
    return 1;
}

int cmd_whoami(gnx::XboxAuth& auth) {
    gnx::XboxProfile profile = auth.fetch_profile();
    std::printf("Gamertag:   %s\nGamerscore: %s\nAvatar:     %s\n",
                profile.gamertag.c_str(), profile.gamerscore.c_str(),
                profile.avatar_url.c_str());
    return 0;
}

// How the account can reach a title, in the same words the library uses.
std::string availability(const gnx::Game& game) {
    std::string label;
    auto add = [&label](const char* text) {
        if (!label.empty()) label += " + ";
        label += text;
    };
    if (game.owned) add("owned");
    if (game.in_subscription) add("in your Game Pass");
    else if (game.subscription) add("needs Game Pass");
    if (game.ad_supported) add("ad-supported");
    return label.empty() ? "unavailable" : label;
}

int cmd_games(gnx::XboxAuth& auth, bool with_names) {
    std::printf("Fetching streaming credentials...\n");
    gnx::StreamingCredentials credentials = auth.fetch_streaming_credentials();
    std::printf("Cloud endpoint: %s\n", credentials.cloud.host.c_str());

    gnx::Http http;
    std::vector<gnx::Game> catalog = gnx::fetch_catalog(http, credentials.cloud);
    std::vector<gnx::Game*> playable;
    for (gnx::Game& game : catalog)
        if (game.playable()) playable.push_back(&game);
    std::printf("Catalog: %zu titles, %zu playable\n\n", catalog.size(),
                playable.size());

    if (with_names) {
        std::printf("Resolving names from the Store catalog...\n\n");
        gnx::fetch_names(http, playable);
    }
    for (const gnx::Game* game : playable) {
        if (game->name.empty())
            std::printf("  %s\n", game->title_id.c_str());
        else
            std::printf("  %-40s  %s\n", game->title_id.c_str(),
                        game->name.c_str());
    }
    return 0;
}

// Searches the whole catalog the way the library's search box does, then
// resolves names for the hits only -- the check that "is X on xCloud, and what
// would I need to play it?" works without downloading 2400 store entries.
int cmd_search(gnx::XboxAuth& auth, const std::string& query) {
    gnx::StreamingCredentials credentials = auth.fetch_streaming_credentials();
    gnx::Http http;
    std::vector<gnx::Game> catalog = gnx::fetch_catalog(http, credentials.cloud);

    std::string key = gnx::search_key(query);
    if (key.empty()) {
        std::printf("Nothing searchable in \"%s\".\n", query.c_str());
        return 1;  // an empty key matches every title -- refuse, don't resolve
    }
    std::vector<gnx::Game*> hits;
    for (gnx::Game& game : catalog)
        if (gnx::matches_query(game, key)) hits.push_back(&game);

    if (hits.empty()) {
        std::printf("\"%s\" is not on xCloud — not with Game Pass, not as a "
                    "game you own.\n", query.c_str());
        return 0;
    }
    std::printf("%zu of %zu titles match \"%s\":\n\n", hits.size(),
                catalog.size(), query.c_str());
    // Each batch of 20 is ~1 MB of store metadata, so a broad query resolves
    // a screenful and lists the rest by id -- same rule as the console UI.
    constexpr size_t kResolveLimit = 40;
    if (hits.size() > kResolveLimit)
        std::printf("(resolving names for the first %zu)\n\n", kResolveLimit);
    std::vector<gnx::Game*> resolve(
        hits.begin(),
        hits.begin() + static_cast<std::ptrdiff_t>(
                           std::min(hits.size(), kResolveLimit)));
    gnx::fetch_names(http, resolve);
    for (const gnx::Game* game : hits)
        std::printf("  %-34s  %-44s  %s\n", game->title_id.c_str(),
                    (game->name.empty() ? "?" : game->name).c_str(),
                    availability(*game).c_str());
    return 0;
}

// Exercises the session signaling end to end (minus WebRTC): create a
// session with the high-quality (tizen) fingerprint, wait for provisioning,
// authenticate the connection, then tear it down.
int cmd_stream_test(gnx::XboxAuth& auth, const std::string& title_id) {
    std::printf("Fetching streaming credentials...\n");
    gnx::StreamingCredentials credentials = auth.fetch_streaming_credentials();
    gnx::Http http;

    std::printf("Cleaning up stale sessions...\n");
    gnx::GssvSession::cleanup_stale_sessions(http, credentials.cloud);

    gnx::GssvSession session(http, credentials.cloud,
                             gnx::QualityTier::P1080HQ);
    std::printf("Starting session for %s (1080p HQ tier)...\n",
                title_id.c_str());
    session.start_cloud(title_id);

    bool connected = false;
    for (int i = 0; i < 240; ++i) {
        gnx::SessionState state = session.refresh_state();
        const char* name = "?";
        switch (state) {
            case gnx::SessionState::New: name = "New"; break;
            case gnx::SessionState::Provisioning: name = "Provisioning"; break;
            case gnx::SessionState::WaitingForResources:
                name = "WaitingForResources"; break;
            case gnx::SessionState::ReadyToConnect:
                name = "ReadyToConnect"; break;
            case gnx::SessionState::Provisioned: name = "Provisioned"; break;
            case gnx::SessionState::Failed: name = "Failed"; break;
        }
        std::printf("  state: %s\n", name);
        if (state == gnx::SessionState::ReadyToConnect && !connected) {
            std::printf("Sending connect (passport token)...\n");
            session.connect(auth.fetch_passport_token());
            connected = true;
        }
        if (state == gnx::SessionState::Provisioned) {
            std::printf(
                "\nSession fully provisioned — signaling works!\n"
                "(WebRTC transport is the remaining native piece.)\n");
            session.stop();
            return 0;
        }
        if (state == gnx::SessionState::Failed) {
            std::printf("Session failed: %s\n",
                        session.error_details().c_str());
            session.stop();
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::printf("Timed out.\n");
    session.stop();
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string command = argc > 1 ? argv[1] : "";
    try {
        gnx::XboxAuth auth(token_store_path());
        if (command == "login") return cmd_login(auth);
        if (command == "logout") { auth.logout(); return 0; }
        if (command == "whoami") return cmd_whoami(auth);
        if (command == "games") return cmd_games(auth, true);
        if (command == "ids") return cmd_games(auth, false);
        if (command == "search" && argc > 2) {
            std::string query = argv[2];
            for (int i = 3; i < argc; ++i) query += std::string(" ") + argv[i];
            return cmd_search(auth, query);
        }
        if (command == "stream-test" && argc > 2)
            return cmd_stream_test(auth, argv[2]);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    std::printf(
        "usage: greennx <login|logout|whoami|games|ids|search <QUERY>|"
        "stream-test <TITLEID>>\n");
    return 2;
}
