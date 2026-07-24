// green-nx: Xbox Cloud Gaming for the Nintendo Switch.
//
// SDL2 frontend: splash -> device-code sign-in -> game library grid with box
// art and search -> native WebRTC streaming (see src/switch/stream/).

#include <SDL2/SDL.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../../vendor/json.hpp"
#include "../core/auth.hpp"
#include "../core/catalog.hpp"
#include "covers.hpp"
#include "gfx.hpp"

#ifdef GNX_NATIVE_STREAM
#include "stream/engine.hpp"
#endif

using nlohmann::json;
using namespace gnx;

namespace {

#ifdef __SWITCH__
constexpr const char* kDataDir = "sdmc:/switch/green-nx";
#else
const std::string kDataDirStr = std::string(getenv("HOME")) + "/.green-nx";
const char* kDataDir = kDataDirStr.c_str();
#endif

std::string data_path(const char* leaf) {
    return std::string(kDataDir) + "/" + leaf;
}

// ---- Switch joystick button indices (libnx SDL2 port) ---------------------
enum JoyButton {
    kBtnA = 0, kBtnB = 1, kBtnX = 2, kBtnY = 3,
    kBtnL = 6, kBtnR = 7, kBtnZL = 8, kBtnZR = 9,
    kBtnPlus = 10, kBtnMinus = 11,
    kBtnLeft = 12, kBtnUp = 13, kBtnRight = 14, kBtnDown = 15,
};

enum class Scene {
    Splash, SignIn, LoadingLibrary, Library, Settings, Stream, Fatal
};

enum class LibraryTab { All, Favorites, History };
constexpr int kTabCount = 3;
constexpr int kHistoryMax = 10;  // recently-played games kept

#ifdef __SWITCH__
// HD-rumble driven straight through libnx. We can't use SDL_JoystickRumble: the
// devkitPro SDL port only initializes vibration handles for HidNpadIdType_No1
// (player 1), never HidNpadIdType_Handheld -- so in handheld mode the send goes
// to a slot with no motor and nothing happens. We instead init both targets
// (two actuators each: left + right) and send to whichever npad is active.
// Unlike SDL, libnx vibration is set-and-hold, so we stop it ourselves once the
// server report's duration elapses (tick()).
struct SwitchRumble {
    HidVibrationDeviceHandle handheld_[2] = {};
    HidVibrationDeviceHandle player1_[2] = {};
    bool ready_ = false;
    bool active_ = false;
    Uint32 expiry_ = 0;

    void init() {
        Result r1 = hidInitializeVibrationDevices(
            handheld_, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        Result r2 = hidInitializeVibrationDevices(
            player1_, 2, HidNpadIdType_No1, HidNpadStyleSet_NpadFullCtrl);
        ready_ = R_SUCCEEDED(r1) && R_SUCCEEDED(r2);
    }

    // Handheld mode -> built-in rails; otherwise the player-1 controller.
    const HidVibrationDeviceHandle* target() const {
        if (hidGetNpadStyleSet(HidNpadIdType_Handheld) &
            HidNpadStyleTag_NpadHandheld)
            return handheld_;
        return player1_;
    }

    void send(float low, float high) {
        auto clamp01 = [](float v) { return v < 0 ? 0.f : (v > 1 ? 1.f : v); };
        HidVibrationValue v[2];
        v[0].amp_low = clamp01(low);
        v[0].freq_low = 160.0f;     // HD-rumble low-band centre
        v[0].amp_high = clamp01(high);
        v[0].freq_high = 320.0f;    // HD-rumble high-band centre
        v[1] = v[0];                // both actuators together
        hidSendVibrationValues(target(), v, 2);
    }

    // Start a burst scaled by the user's intensity gain; auto-stops after
    // duration_ms (floored so a 0-length report is still felt, and the server
    // re-sends to sustain longer effects). The high band gets an extra trim --
    // it is inherently louder/harsher and is what you hear humming.
    void play(float low, float high, float gain, Uint32 duration_ms,
              Uint32 now) {
        if (!ready_) return;
        float lo = low * gain;
        float hi = high * gain * 0.75f;
        send(lo, hi);
        active_ = lo > 0.0f || hi > 0.0f;
        expiry_ = now + (duration_ms ? duration_ms : 150);
    }

    void tick(Uint32 now) {
        if (ready_ && active_ && static_cast<Sint32>(now - expiry_) >= 0) {
            send(0.0f, 0.0f);
            active_ = false;
        }
    }

    void stop() {
        if (ready_) send(0.0f, 0.0f);
        active_ = false;
    }
};
#endif

struct Settings {
    int quality = 2;    // 0=720p, 1=1080p, 2=1080p HQ
    int pacing = 0;     // 0=low latency, 1=smooth
    int sharpness = 2;  // 0..4 fixed strength, 5=strobe test
    int contrast = 0;   // 0..3 fixed strength, 4=strobe test
    int mapping = 0;    // 0=positional, 1=match labels
    int vibration = 2;  // rumble intensity: 0=Off, 1=Low, 2=Medium, 3=High
    int region = 0;     // region-bypass IP: 0=Off, else index into kRegion*
    int language = 0;   // index into kLanguage* (0 = English US)
};

constexpr int kLanguageCount = 14;
constexpr int kVibrationLevels = 4;

Settings load_settings();
void save_settings(const Settings& settings);

struct App {
    gfx::Gfx gfx;
    std::unique_ptr<Covers> covers;
    std::unique_ptr<XboxAuth> auth;

    Scene scene = Scene::Splash;
    Uint32 scene_started = 0;
    std::string status;   // progress / error line
    std::string fatal;

    // sign-in
    DeviceCode device_code;
    std::atomic<int> signin_state{0};  // 0 running, 1 ok, 2 restart, 3 error
    std::string signin_error;
    std::thread worker;
    std::atomic<bool> abort_http{false};  // exit: unblock worker HTTP calls

    // library
    std::vector<Game> games;
    std::vector<int> visible;  // indices into games for the active tab + search
    std::string query;
    int cursor = 0;
    LibraryTab tab = LibraryTab::All;
    std::vector<std::string> favorites;  // title_ids, marked by the user
    std::vector<std::string> history;    // title_ids, most-recent first
    std::atomic<int> load_state{0};  // 0 running, 1 ok, 2 error
    std::string load_error;
    std::string gamertag;
    Game launch_game;
    Settings settings;
    int settings_cursor = 0;
    Scene settings_return = Scene::Library;  // scene to go back to from Settings

#ifdef GNX_NATIVE_STREAM
    std::unique_ptr<stream::Engine> engine;
    Uint32 stream_hint_until = 0;
    bool deko_active = false;  // deko3d owns the display (SDL suspended)
    Uint32 last_input_ms = 0;  // input pacing during deko3d streaming
#ifdef __SWITCH__
    SwitchRumble rumble;  // server vibration reports -> HD rumble
#endif
#endif
};

Settings load_settings() {
    Settings settings;
    std::ifstream in(data_path("settings.json"));
    if (!in) return settings;
    json data = json::parse(in, nullptr, false);
    if (data.is_discarded()) return settings;
    settings.quality = std::clamp(data.value("quality", 2), 0, 2);
    settings.pacing = std::clamp(data.value("pacing", 0), 0, 1);
    settings.sharpness = std::clamp(data.value("sharpness", 2), 0, 5);
    settings.contrast = std::clamp(data.value("contrast", 0), 0, 4);
    settings.mapping = std::clamp(data.value("mapping", 0), 0, 1);
    // "vibration" was an on/off bool before intensity levels existed; migrate.
    if (data.contains("vibration") && data["vibration"].is_boolean())
        settings.vibration = data["vibration"].get<bool>() ? 2 : 0;
    else
        settings.vibration =
            std::clamp(data.value("vibration", 2), 0, kVibrationLevels - 1);
    settings.region = std::clamp(data.value("region", 0), 0, 5);
    settings.language =
        std::clamp(data.value("language", 0), 0, kLanguageCount - 1);
    return settings;
}

void save_settings(const Settings& settings) {
    std::ofstream out(data_path("settings.json"), std::ios::trunc);
    out << json{{"quality", settings.quality},
                {"pacing", settings.pacing},
                {"sharpness", settings.sharpness},
                {"contrast", settings.contrast},
                {"mapping", settings.mapping},
                {"vibration", settings.vibration},
                {"region", settings.region},
                {"language", settings.language}}.dump(2);
}

// Streamed console's system language (BCP-47). Games without an in-game
// language menu inherit this; sent as the session "locale". Native labels are
// limited to scripts the Switch Standard shared font can render (Latin,
// Cyrillic, Japanese) -- Korean/Chinese need fonts we don't load.
const char* kLanguageLabels[kLanguageCount] = {
    "English (US)", "English (UK)", "Español (España)", "Español (México)",
    "Français", "Deutsch", "Italiano", "Português (Brasil)",
    "Português (Portugal)", "Polski", "Nederlands", "Türkçe",
    "Russian", "Japanese"};
const char* kLanguageCodes[kLanguageCount] = {
    "en-US", "en-GB", "es-ES", "es-MX", "fr-FR", "de-DE", "it-IT",
    "pt-BR", "pt-PT", "pl-PL", "nl-NL", "tr-TR", "ru-RU", "ja-JP"};

// Rumble intensity. HD rumble at amplitude 1.0 is very strong and audibly hums,
// so even "High" leaves headroom rather than driving the actuators flat out.
const char* kVibrationLabels[kVibrationLevels] = {"Off", "Low", "Medium",
                                                  "High"};
const float kVibrationGain[kVibrationLevels] = {0.0f, 0.35f, 0.6f, 0.9f};

// Region bypass: spoof a supported-region IP via X-Forwarded-For so xCloud's
// geo gate opens for accounts outside the officially supported countries. IPs
// are the known-good values shipped by better-xcloud. Index 0 = disabled.
const char* kRegionLabels[6] = {"Off", "United States", "Brazil",
                                "Japan", "Korea", "Poland"};
const char* kRegionIps[6] = {"", "143.244.47.65", "169.150.198.66",
                             "138.199.21.239", "121.125.60.151",
                             "45.134.212.66"};

void apply_region(const Settings& settings) {
    Http::set_forwarded_for(kRegionIps[std::clamp(settings.region, 0, 5)]);
}

// ---- persistence ----------------------------------------------------------

constexpr int kGamesCacheVersion = 2;  // v1 lacked boxArt: force a refresh

void save_games_cache(const std::vector<Game>& games) {
    json list = json::array();
    for (const Game& game : games)
        list.push_back({{"titleId", game.title_id},
                        {"productId", game.product_id},
                        {"name", game.name},
                        {"boxArt", game.box_art_url}});
    std::ofstream out(data_path("games.json"), std::ios::trunc);
    out << json{{"version", kGamesCacheVersion}, {"games", list}}.dump();
}

std::vector<Game> load_games_cache() {
    std::vector<Game> games;
    std::ifstream in(data_path("games.json"));
    if (!in) return games;
    json data = json::parse(in, nullptr, false);
    if (data.is_discarded() || !data.is_object() ||
        data.value("version", 0) < kGamesCacheVersion)
        return games;  // stale or old-format cache -> full refresh
    for (const json& entry : data.value("games", json::array())) {
        Game game;
        game.title_id = entry.value("titleId", "");
        game.product_id = entry.value("productId", "");
        game.name = entry.value("name", "");
        game.box_art_url = entry.value("boxArt", "");
        if (!game.title_id.empty()) games.push_back(std::move(game));
    }
    return games;
}

// Favorites and history are stored as plain title-id lists (JSON arrays).
std::vector<std::string> load_id_list(const char* leaf) {
    std::vector<std::string> ids;
    std::ifstream in(data_path(leaf));
    if (!in) return ids;
    json data = json::parse(in, nullptr, false);
    if (!data.is_array()) return ids;
    for (const json& entry : data)
        if (entry.is_string()) ids.push_back(entry.get<std::string>());
    return ids;
}

void save_id_list(const char* leaf, const std::vector<std::string>& ids) {
    std::ofstream out(data_path(leaf), std::ios::trunc);
    out << json(ids).dump();
}

bool is_favorite(const App& app, const std::string& id) {
    return std::find(app.favorites.begin(), app.favorites.end(), id) !=
           app.favorites.end();
}

void toggle_favorite(App& app, const std::string& id) {
    auto it = std::find(app.favorites.begin(), app.favorites.end(), id);
    if (it != app.favorites.end())
        app.favorites.erase(it);
    else
        app.favorites.push_back(id);
    save_id_list("favorites.json", app.favorites);
}

// Record a launch: move the title to the front, dedup, cap at kHistoryMax.
void push_history(App& app, const std::string& id) {
    auto it = std::find(app.history.begin(), app.history.end(), id);
    if (it != app.history.end()) app.history.erase(it);
    app.history.insert(app.history.begin(), id);
    if (static_cast<int>(app.history.size()) > kHistoryMax)
        app.history.resize(kHistoryMax);
    save_id_list("history.json", app.history);
}

// ---- background work ------------------------------------------------------

void start_signin(App& app) {
    app.signin_state = 0;
    app.worker = std::thread([&app] {
        try {
            app.device_code = app.auth->request_device_code();
        } catch (const std::exception& error) {
            app.signin_error = error.what();
            app.signin_state = 3;
            return;
        }
        while (app.signin_state == 0) {
            // Sleep in short slices so a cancel (exit) doesn't have to wait
            // out the full poll interval (5-15 s) before the join returns.
            Uint32 wait_ms = static_cast<Uint32>(
                std::max(app.device_code.interval_secs, 1) * 1000);
            for (Uint32 waited = 0; waited < wait_ms && app.signin_state == 0;
                 waited += 100)
                SDL_Delay(100);
            if (app.signin_state != 0) return;
            try {
                switch (app.auth->poll_device_code(app.device_code)) {
                    case PollResult::Authorized: app.signin_state = 1; return;
                    case PollResult::Expired:    app.signin_state = 2; return;
                    case PollResult::Pending:    break;
                }
            } catch (const std::exception& error) {
                app.signin_error = error.what();
                app.signin_state = 3;
                return;
            }
        }
    });
}

void start_library_load(App& app, bool force_refresh) {
    app.load_state = 0;
    app.status = "Connecting to Xbox...";
    app.worker = std::thread([&app, force_refresh] {
        try {
            if (!force_refresh) {
                std::vector<Game> cached = load_games_cache();
                if (!cached.empty()) {
                    app.games = std::move(cached);
                    app.load_state = 1;
                    return;
                }
            }
            app.status = "Fetching streaming credentials...";
            StreamingCredentials credentials =
                app.auth->fetch_streaming_credentials();
            app.status = "Loading your library...";
            Http http;
            std::vector<Game> games =
                fetch_playable_titles(http, credentials.cloud);
            app.status = "Resolving names and covers (" +
                         std::to_string(games.size()) + " titles)...";
            fetch_names(http, games);
            std::sort(games.begin(), games.end(),
                      [](const Game& a, const Game& b) {
                          const std::string& left =
                              a.name.empty() ? a.title_id : a.name;
                          const std::string& right =
                              b.name.empty() ? b.title_id : b.name;
                          return left < right;
                      });
            save_games_cache(games);
            app.games = std::move(games);
            app.load_state = 1;
        } catch (const std::exception& error) {
            app.load_error = error.what();
            app.load_state = 2;
        }
    });
}

void join_worker(App& app) {
    if (app.worker.joinable()) app.worker.join();
}

// ---- helpers --------------------------------------------------------------

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return value;
}

int find_game(const App& app, const std::string& id) {
    for (int i = 0; i < static_cast<int>(app.games.size()); ++i)
        if (app.games[i].title_id == id) return i;
    return -1;
}

// Rebuild `visible` for the active tab, honouring the search query. All /
// Favorites keep the library's alphabetical order; History keeps recency order.
void apply_filter(App& app) {
    app.visible.clear();
    std::string needle = lowercase(app.query);
    auto matches = [&](const Game& game) {
        return needle.empty() ||
               lowercase(game.name).find(needle) != std::string::npos ||
               lowercase(game.title_id).find(needle) != std::string::npos;
    };

    if (app.tab == LibraryTab::History) {
        for (const std::string& id : app.history) {
            int i = find_game(app, id);
            if (i >= 0 && matches(app.games[i])) app.visible.push_back(i);
        }
    } else {
        for (int i = 0; i < static_cast<int>(app.games.size()); ++i) {
            if (app.tab == LibraryTab::Favorites &&
                !is_favorite(app, app.games[i].title_id))
                continue;
            if (matches(app.games[i])) app.visible.push_back(i);
        }
    }
    app.cursor = std::min(app.cursor,
                          std::max(0, static_cast<int>(app.visible.size()) - 1));
}

std::string keyboard_input(const std::string& initial) {
#ifdef __SWITCH__
    SwkbdConfig keyboard;
    char buffer[256] = {};
    if (R_FAILED(swkbdCreate(&keyboard, 0))) return initial;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetGuideText(&keyboard, "Search your library");
    swkbdConfigSetInitialText(&keyboard, initial.c_str());
    Result rc = swkbdShow(&keyboard, buffer, sizeof(buffer));
    swkbdClose(&keyboard);
    return R_SUCCEEDED(rc) ? std::string(buffer) : initial;
#else
    return initial.empty() ? "halo" : "";  // desktop stub for testing
#endif
}

// ---- scenes ---------------------------------------------------------------

void draw_footer(App& app, const std::string& hints) {
    app.gfx.fill({0, gfx::kHeight - 70, gfx::kWidth, 70}, gfx::kBgAccent);
    app.gfx.text(hints, 60, gfx::kHeight - 55, gfx::FontSize::Small,
                 gfx::kTextDim);
}

void draw_splash(App& app) {
    Uint32 ticks = SDL_GetTicks();
    app.gfx.text_centered("green-nx", gfx::kWidth / 2, 380,
                          gfx::FontSize::Huge, gfx::kText);
    app.gfx.text_centered("Xbox Cloud Gaming for Nintendo Switch",
                          gfx::kWidth / 2, 510, gfx::FontSize::Body,
                          gfx::kTextDim);
    app.gfx.fill({gfx::kWidth / 2 - 120, 350, 240, 6}, gfx::kAccent);
    app.gfx.spinner(gfx::kWidth / 2, 640, ticks);
}

void draw_signin(App& app) {
    app.gfx.text_centered("Sign in with Microsoft", gfx::kWidth / 2, 180,
                          gfx::FontSize::Title, gfx::kText);
    if (app.device_code.user_code.empty()) {
        app.gfx.spinner(gfx::kWidth / 2, 480, SDL_GetTicks());
        return;
    }
    app.gfx.text_centered("On your phone or computer, open",
                          gfx::kWidth / 2, 330, gfx::FontSize::Body,
                          gfx::kTextDim);
    app.gfx.text_centered(app.device_code.verification_uri, gfx::kWidth / 2,
                          390, gfx::FontSize::Title, gfx::kText);
    app.gfx.text_centered("and enter this code", gfx::kWidth / 2, 500,
                          gfx::FontSize::Body, gfx::kTextDim);

    int code_width = app.gfx.text_width(app.device_code.user_code,
                                        gfx::FontSize::Huge);
    SDL_Rect box = {gfx::kWidth / 2 - code_width / 2 - 50, 560,
                    code_width + 100, 150};
    app.gfx.fill(box, gfx::kCard);
    app.gfx.frame(box, gfx::kAccent, 4);
    app.gfx.text_centered(app.device_code.user_code, gfx::kWidth / 2, 585,
                          gfx::FontSize::Huge, gfx::kText);
    app.gfx.spinner(gfx::kWidth / 2, 780, SDL_GetTicks());
    draw_footer(app, "Waiting for sign-in...   ZL  Settings   B  Exit");
}

void draw_loading(App& app) {
    app.gfx.text_centered("green-nx", gfx::kWidth / 2, 380,
                          gfx::FontSize::Huge, gfx::kText);
    app.gfx.text_centered(app.status, gfx::kWidth / 2, 560,
                          gfx::FontSize::Body, gfx::kTextDim);
    app.gfx.spinner(gfx::kWidth / 2, 640, SDL_GetTicks());
}

// Grid geometry
constexpr int kColumns = 6;
constexpr int kCardW = 260;
constexpr int kCardH = 390;
constexpr int kGapX = 34;
constexpr int kGapY = 88;
constexpr int kGridX = (gfx::kWidth - kColumns * kCardW -
                        (kColumns - 1) * kGapX) / 2;
constexpr int kGridY = 150;
constexpr int kRowsVisible = 2;

const char* kTabNames[kTabCount] = {"All games", "Favorites", "History"};

void draw_library(App& app) {
    // Header: title, tab bar, gamertag, count/search.
    app.gfx.text("green-nx", 60, 40, gfx::FontSize::Title, gfx::kText);
    if (!app.gamertag.empty())
        app.gfx.text(app.gamertag,
                     gfx::kWidth - 60 -
                         app.gfx.text_width(app.gamertag,
                                            gfx::FontSize::Body),
                     50, gfx::FontSize::Body, gfx::kTextDim);

    int tx = 60;
    for (int t = 0; t < kTabCount; ++t) {
        bool active = static_cast<int>(app.tab) == t;
        int w = app.gfx.text(kTabNames[t], tx, 100, gfx::FontSize::Body,
                             active ? gfx::kText : gfx::kTextDim);
        if (active) app.gfx.fill({tx, 142, w, 4}, gfx::kAccent);
        tx += w + 50;
    }
    std::string info = std::to_string(app.visible.size()) + " games";
    if (!app.query.empty()) info += "   ·   \"" + app.query + "\"";
    app.gfx.text(info,
                 gfx::kWidth - 60 - app.gfx.text_width(info, gfx::FontSize::Small),
                 112, gfx::FontSize::Small, gfx::kTextDim);

    if (app.visible.empty()) {
        std::string msg;
        if (!app.query.empty())
            msg = "Nothing found for \"" + app.query + "\"";
        else if (app.tab == LibraryTab::Favorites)
            msg = "No favorites yet - press X on a game to add it";
        else if (app.tab == LibraryTab::History)
            msg = "Nothing played yet - your recent games will appear here";
        else
            msg = "No games available for this account";
        app.gfx.text_centered(msg, gfx::kWidth / 2, 480, gfx::FontSize::Body,
                              gfx::kTextDim);
    }

    int first_row = std::max(0, app.cursor / kColumns - (kRowsVisible - 1));
    for (int slot = 0; slot < kColumns * (kRowsVisible + 1); ++slot) {
        int index = first_row * kColumns + slot;
        if (index >= static_cast<int>(app.visible.size())) break;
        const Game& game = app.games[app.visible[index]];

        int column = slot % kColumns;
        int row = slot / kColumns;
        SDL_Rect card = {kGridX + column * (kCardW + kGapX),
                         kGridY + row * (kCardH + kGapY), kCardW, kCardH};
        if (card.y > gfx::kHeight - 80) break;

        SDL_Texture* cover =
            app.covers->get(game.title_id, game.box_art_url);
        if (cover) {
            app.gfx.draw_texture(cover, card);
        } else {
            app.gfx.fill(card, gfx::kCard);
            const std::string& label =
                game.name.empty() ? game.title_id : game.name;
            app.gfx.text_centered(label.substr(0, 18), card.x + kCardW / 2,
                                  card.y + kCardH / 2 - 20,
                                  gfx::FontSize::Small, gfx::kTextDim);
        }

        // Favorite marker: a gold badge (with a star glyph on top) top-left.
        // The badge alone reads as "marked" even if the font lacks the glyph.
        if (is_favorite(app, game.title_id)) {
            SDL_Rect badge = {card.x + 8, card.y + 8, 50, 50};
            app.gfx.fill(badge, gfx::kWarn);
            app.gfx.text_centered("★", badge.x + 25, badge.y + 4,
                                  gfx::FontSize::Body, gfx::kBg);
        }

        if (index == app.cursor) {
            app.gfx.frame(card, gfx::kCardFocus, 5);
            const std::string& label =
                game.name.empty() ? game.title_id : game.name;
            app.gfx.text_centered(label.substr(0, 30), card.x + kCardW / 2,
                                  card.y + kCardH + 14,
                                  gfx::FontSize::Small, gfx::kText);
        }
    }

    draw_footer(app,
                "A  Play   X  Favorite   L/R  Tabs   Y  Search   "
                "ZR  Refresh   ZL  Settings   -  Sign out   +  Exit");
}

const char* kQualityLabels[3] = {"720p", "1080p", "1080p high bitrate"};
const char* kPacingLabels[2] = {"Low latency", "Smooth"};
const char* kSharpnessLabels[6] = {
    "Off", "Low", "Medium", "High", "Extreme", "Strobe test"};
const char* kContrastLabels[5] = {
    "Off", "Low", "Medium", "High", "Strobe test"};
const char* kMappingLabels[2] = {"Positional (Switch A = Xbox B)",
                                 "Match labels (Switch A = Xbox A)"};

void draw_settings(App& app) {
    app.gfx.text("Settings", 60, 40, gfx::FontSize::Title, gfx::kText);

    struct Row {
        const char* title;
        std::string value;
    };
    Row rows[8] = {
        {"Stream quality", kQualityLabels[app.settings.quality]},
        {"Video pacing", kPacingLabels[app.settings.pacing]},
        {"Luma sharpening", kSharpnessLabels[app.settings.sharpness]},
        {"Contrast", kContrastLabels[app.settings.contrast]},
        {"Button layout", kMappingLabels[app.settings.mapping]},
        {"Vibration", kVibrationLabels[app.settings.vibration]},
        {"Region bypass", kRegionLabels[app.settings.region]},
        {"Game language", kLanguageLabels[app.settings.language]},
    };
    for (int i = 0; i < 8; ++i) {
        SDL_Rect row = {120, 70 + i * 82, gfx::kWidth - 240, 64};
        app.gfx.fill(row, gfx::kCard);
        if (i == app.settings_cursor) app.gfx.frame(row, gfx::kCardFocus, 4);
        app.gfx.text(rows[i].title, row.x + 40, row.y + 12,
                     gfx::FontSize::Body, gfx::kText);
        app.gfx.text(rows[i].value,
                     row.x + row.w - 40 -
                         app.gfx.text_width(rows[i].value,
                                            gfx::FontSize::Body),
                     row.y + 12, gfx::FontSize::Body, gfx::kAccent);
    }
    const char* note_line_1;
    const char* note_line_2 = nullptr;
    if (app.settings_cursor == 7) {
        note_line_1 =
            "Sets the streamed console's language for games without a "
            "language menu.";
        note_line_2 = "The change takes effect the next time a game launches.";
    } else if (app.settings_cursor == 1) {
        note_line_1 =
            "Low latency shows the newest frame immediately. Smooth keeps one "
            "frame in reserve";
        note_line_2 =
            "for steadier motion, with about one source frame of added delay.";
    } else if (app.settings_cursor == 2) {
        note_line_1 =
            "Medium is recommended. Sharpening restores brightness detail; "
            "Off keeps the source image.";
        note_line_2 =
            "Strobe test cycles Off, Low, Medium, High and Extreme every 3 "
            "seconds and labels each mode.";
    } else if (app.settings_cursor == 3) {
        note_line_1 =
            "Medium is recommended. Contrast adds depth while protecting "
            "black and white endpoints.";
        note_line_2 =
            "Strobe test cycles Off, Low, Medium and High every 3 seconds and "
            "labels each mode.";
    } else if (app.settings.region != 0) {
        note_line_1 =
            "Region bypass spoofs your location to Xbox to reach xCloud from "
            "an unsupported country.";
        note_line_2 = "Use your own account at your own risk.";
    } else {
        note_line_1 =
            "Higher quality needs a stronger connection. Use 5 GHz Wi-Fi or "
            "docked LAN";
        note_line_2 = "for 1080p high bitrate.";
    }
    const int note_y = note_line_2 ? 775 : 800;
    app.gfx.text_centered(note_line_1, gfx::kWidth / 2, note_y,
                          gfx::FontSize::Small, gfx::kTextDim);
    if (note_line_2)
        app.gfx.text_centered(note_line_2, gfx::kWidth / 2, note_y + 34,
                              gfx::FontSize::Small, gfx::kTextDim);
    draw_footer(app, "Left / Right  Change   B  Back");
}

#ifdef GNX_NATIVE_STREAM
// mapping 0: positional (Switch east button -> Xbox east button).
// mapping 1: match labels (Switch A -> Xbox A).
xcloud::GamepadFrame read_gamepad(SDL_Joystick* joystick, int mapping) {
    xcloud::GamepadFrame frame;
    auto button = [&](int index) {
        return SDL_JoystickGetButton(joystick, index) != 0;
    };
    if (mapping == 0) {
        frame.b = button(kBtnA);       // Switch A (east)  -> Xbox B
        frame.a = button(kBtnB);       // Switch B (south) -> Xbox A
        frame.y = button(kBtnX);       // Switch X (north) -> Xbox Y
        frame.x = button(kBtnY);       // Switch Y (west)  -> Xbox X
    } else {
        frame.a = button(kBtnA);
        frame.b = button(kBtnB);
        frame.x = button(kBtnX);
        frame.y = button(kBtnY);
    }
    frame.left_shoulder = button(kBtnL);
    frame.right_shoulder = button(kBtnR);
    frame.left_trigger = button(kBtnZL) ? 1.0f : 0.0f;
    frame.right_trigger = button(kBtnZR) ? 1.0f : 0.0f;
    frame.menu = button(kBtnPlus);
    frame.view = button(kBtnMinus);
    frame.left_thumb = button(4);
    frame.right_thumb = button(5);
    frame.dpad_left = button(kBtnLeft);
    frame.dpad_up = button(kBtnUp);
    frame.dpad_right = button(kBtnRight);
    frame.dpad_down = button(kBtnDown);
    // Both stick clicks together = Xbox nexus (guide).
    if (frame.left_thumb && frame.right_thumb) {
        frame.nexus = true;
        frame.left_thumb = frame.right_thumb = false;
    }
    auto axis = [&](int index) {
        return SDL_JoystickGetAxis(joystick, index) / 32767.0f;
    };
    frame.left_x = axis(0);
    frame.left_y = axis(1);
    frame.right_x = axis(2);
    frame.right_y = axis(3);
    return frame;
}

// Drain the newest rumble command from the engine and drive the motors, then
// service the auto-stop timer. Runs every frame on the main thread. The setting
// gates it: when vibration is off we stop and never start. See SwitchRumble for
// why this goes through libnx instead of SDL_JoystickRumble.
void apply_rumble(App& app) {
#ifdef __SWITCH__
    Uint32 now = SDL_GetTicks();
    float gain = kVibrationGain[std::clamp(app.settings.vibration, 0,
                                           kVibrationLevels - 1)];
    stream::Engine::RumbleCommand cmd;
    if (app.engine->take_rumble(cmd)) {
        if (gain > 0.0f)
            app.rumble.play(cmd.low / 65535.0f, cmd.high / 65535.0f, gain,
                            cmd.duration_ms, now);
        else
            app.rumble.stop();
    }
    app.rumble.tick(now);
#else
    stream::Engine::RumbleCommand cmd;
    (void)app.engine->take_rumble(cmd);  // PC: no rumble hardware
#endif
}

void draw_stream(App& app, SDL_Joystick* joystick) {
    stream::EngineState state = app.engine->state();

    if (state == stream::EngineState::Failed) {
        app.gfx.text_centered("Stream failed", gfx::kWidth / 2, 360,
                              gfx::FontSize::Title, gfx::kError);
        app.gfx.text_centered(app.engine->error().substr(0, 90),
                              gfx::kWidth / 2, 480, gfx::FontSize::Body,
                              gfx::kText);
        app.gfx.text_centered(
            "Details: /switch/green-nx/stream-log.txt on your SD card",
            gfx::kWidth / 2, 560, gfx::FontSize::Small, gfx::kTextDim);
        draw_footer(app, "A  Retry   B  Back to library");
        return;
    }

    SDL_Texture* frame = app.engine->pump_video();
    if (frame) {
        // Letterbox to preserve aspect.
        int width = app.engine->video_width();
        int height = app.engine->video_height();
        SDL_Rect destination = {0, 0, gfx::kWidth, gfx::kHeight};
        if (width > 0 && height > 0) {
            float scale = std::min(
                static_cast<float>(gfx::kWidth) / width,
                static_cast<float>(gfx::kHeight) / height);
            destination.w = static_cast<int>(width * scale);
            destination.h = static_cast<int>(height * scale);
            destination.x = (gfx::kWidth - destination.w) / 2;
            destination.y = (gfx::kHeight - destination.h) / 2;
        }
        app.gfx.draw_texture(frame, destination);
        app.engine->send_gamepad(
            read_gamepad(joystick, app.settings.mapping));
        apply_rumble(app);

        if (SDL_GetTicks() < app.stream_hint_until)
            app.gfx.text_centered(
                "Hold  -  and  +  together to leave the stream",
                gfx::kWidth / 2, gfx::kHeight - 60, gfx::FontSize::Small,
                gfx::kTextDim);
        return;
    }

    const std::string& label =
        app.launch_game.name.empty() ? app.launch_game.title_id
                                     : app.launch_game.name;
    app.gfx.text_centered(label, gfx::kWidth / 2, 400, gfx::FontSize::Title,
                          gfx::kText);
    app.gfx.text_centered(app.engine->status(), gfx::kWidth / 2, 520,
                          gfx::FontSize::Body, gfx::kTextDim);
    app.gfx.spinner(gfx::kWidth / 2, 620, SDL_GetTicks());
    draw_footer(app, "B  Cancel");
}
#endif

void draw_fatal(App& app) {
    app.gfx.text_centered("Something went wrong", gfx::kWidth / 2, 340,
                          gfx::FontSize::Title, gfx::kError);
    app.gfx.text_centered(app.fatal.substr(0, 90), gfx::kWidth / 2, 470,
                          gfx::FontSize::Body, gfx::kText);
    if (app.fatal.size() > 90)
        app.gfx.text_centered(app.fatal.substr(90, 90), gfx::kWidth / 2, 520,
                              gfx::FontSize::Body, gfx::kText);
    // The streaming-login step is the geo gate; if it failed, point the user at
    // the Region bypass setting rather than leaving them at a dead end.
    if (app.fatal.find("streaming login") != std::string::npos)
        app.gfx.text_centered(
            "Xbox Cloud Gaming may be unavailable in your region. Press ZL for "
            "Settings, turn on Region bypass, then press X to retry.",
            gfx::kWidth / 2, 610, gfx::FontSize::Small, gfx::kTextDim);
    draw_footer(app, "X  Retry   ZL  Settings   -  Sign out   +  Exit");
}

// ---- input ----------------------------------------------------------------

struct Input {
    bool a = false, b = false, x = false, y = false;
    bool up = false, down = false, left = false, right = false;
    bool plus = false, minus = false, zl = false, zr = false;
    bool l = false, r = false;
    bool quit = false;
};

Input poll_input(SDL_Joystick* joystick) {
    Input input;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) input.quit = true;
        if (event.type == SDL_JOYBUTTONDOWN) {
            switch (event.jbutton.button) {
                case kBtnA: input.a = true; break;
                case kBtnB: input.b = true; break;
                case kBtnX: input.x = true; break;
                case kBtnY: input.y = true; break;
                case kBtnUp: input.up = true; break;
                case kBtnDown: input.down = true; break;
                case kBtnLeft: input.left = true; break;
                case kBtnRight: input.right = true; break;
                case kBtnPlus: input.plus = true; break;
                case kBtnMinus: input.minus = true; break;
                case kBtnZL: input.zl = true; break;
                case kBtnZR: input.zr = true; break;
                case kBtnL: input.l = true; break;
                case kBtnR: input.r = true; break;
            }
        }
        if (event.type == SDL_KEYDOWN) {  // desktop testing
            switch (event.key.keysym.sym) {
                case SDLK_RETURN: input.a = true; break;
                case SDLK_ESCAPE: input.plus = true; break;
                case SDLK_UP: input.up = true; break;
                case SDLK_DOWN: input.down = true; break;
                case SDLK_LEFT: input.left = true; break;
                case SDLK_RIGHT: input.right = true; break;
                case SDLK_s: input.y = true; break;
                case SDLK_f: input.x = true; break;   // favorite
                case SDLK_r: input.zr = true; break;  // refresh
                case SDLK_LEFTBRACKET: input.l = true; break;
                case SDLK_RIGHTBRACKET: input.r = true; break;
            }
        }
    }
    // Left analog stick also drives menu navigation: emit one directional
    // step each time the stick crosses into a deflected zone (matches the
    // one-per-press behaviour of the d-pad).
    if (joystick) {
        static int last_x = 0, last_y = 0;
        constexpr int kThreshold = 16000;
        int ax = SDL_JoystickGetAxis(joystick, 0);
        int ay = SDL_JoystickGetAxis(joystick, 1);
        int dx = ax > kThreshold ? 1 : (ax < -kThreshold ? -1 : 0);
        int dy = ay > kThreshold ? 1 : (ay < -kThreshold ? -1 : 0);
        if (dx > 0 && last_x <= 0) input.right = true;
        if (dx < 0 && last_x >= 0) input.left = true;
        if (dy > 0 && last_y <= 0) input.down = true;
        if (dy < 0 && last_y >= 0) input.up = true;
        last_x = dx;
        last_y = dy;
    }
    return input;
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

#ifdef __SWITCH__
    // The default UDP receive buffer is ~42 KB, too small to absorb a 1080p
    // keyframe burst. Bump only udp_rx_buf_size (to 512 KB) and keep everything
    // else at default so the transfer-memory total stays within the bsd:u
    // service limit -- an oversized config makes socketInitialize() fail. If it
    // still fails, tear down cleanly and fall back to the guaranteed default,
    // otherwise no socket gets created at all (sendto-before-init).
    {
        SocketInitConfig cfg = *socketGetDefaultInitConfig();
        cfg.udp_rx_buf_size = 0x80000;  // 512 KB (default ~42 KB)
        if (R_FAILED(socketInitialize(&cfg))) {
            socketExit();
            socketInitializeDefault();
        }
    }
    plInitialize(PlServiceType_User);
    if (R_SUCCEEDED(romfsInit())) Http::set_ca_bundle("romfs:/cacert.pem");
#endif
    mkdir(kDataDir, 0755);

    App app;
    if (!app.gfx.init()) return 1;
    SDL_Joystick* joystick = SDL_JoystickOpen(0);
    app.covers = std::make_unique<Covers>(app.gfx, data_path("covers"));
    app.auth = std::make_unique<XboxAuth>(data_path("tokens.json"));
    app.auth->set_abort_flag(&app.abort_http);
    app.settings = load_settings();
    app.favorites = load_id_list("favorites.json");
    app.history = load_id_list("history.json");
    apply_region(app.settings);  // before any network: gate opens on first call
#if defined(__SWITCH__) && defined(GNX_NATIVE_STREAM)
    app.rumble.init();  // HID is up (joystick opened) -> get vibration handles
#endif
    app.scene_started = SDL_GetTicks();
#ifdef GNX_NATIVE_STREAM
    app.engine =
        std::make_unique<stream::Engine>(*app.auth, app.gfx.renderer());
#endif

    bool running = true;
    while (running) {
        Input input = poll_input(joystick);
        if (input.quit) break;

        switch (app.scene) {
            case Scene::Splash:
                if (SDL_GetTicks() - app.scene_started > 1200) {
                    if (app.auth->has_saved_login()) {
                        app.scene = Scene::LoadingLibrary;
                        start_library_load(app, false);
                    } else {
                        app.scene = Scene::SignIn;
                        start_signin(app);
                    }
                }
                break;

            case Scene::SignIn:
                if (input.b || input.plus) {
                    app.signin_state = 4;  // cancel
                    app.abort_http = true;  // unblock an in-flight poll
                    join_worker(app);
                    running = false;
                    break;
                }
                if (input.zl) {  // reach Region bypass before the library loads
                    app.settings_return = Scene::SignIn;
                    app.scene = Scene::Settings;
                    break;
                }
                if (app.signin_state == 1) {
                    join_worker(app);
                    app.scene = Scene::LoadingLibrary;
                    start_library_load(app, false);
                } else if (app.signin_state == 2) {
                    join_worker(app);
                    start_signin(app);
                } else if (app.signin_state == 3) {
                    join_worker(app);
                    app.fatal = app.signin_error;
                    app.scene = Scene::Fatal;
                }
                break;

            case Scene::LoadingLibrary:
                if (app.load_state == 1) {
                    join_worker(app);
                    apply_filter(app);
                    app.scene = Scene::Library;
                    try {
                        app.gamertag = app.auth->fetch_profile().gamertag;
                    } catch (const std::exception&) {}
                } else if (app.load_state == 2) {
                    join_worker(app);
                    app.fatal = app.load_error;
                    app.scene = Scene::Fatal;
                }
                break;

            case Scene::Library: {
                int step = 0;
                if (input.right) step = 1;
                if (input.left) step = -1;
                if (input.down) step = kColumns;
                if (input.up) step = -kColumns;
                if (step != 0 && !app.visible.empty()) {
                    app.cursor = std::clamp(
                        app.cursor + step, 0,
                        static_cast<int>(app.visible.size()) - 1);
                }
                if (input.l || input.r) {  // switch tab
                    int t = (static_cast<int>(app.tab) + (input.r ? 1 : -1) +
                             kTabCount) % kTabCount;
                    app.tab = static_cast<LibraryTab>(t);
                    app.cursor = 0;
                    apply_filter(app);
                }
                if (input.x && !app.visible.empty()) {  // toggle favorite
                    toggle_favorite(
                        app, app.games[app.visible[app.cursor]].title_id);
                    apply_filter(app);  // Favorites tab updates live
                }
                if (input.y) {
                    app.query = keyboard_input(app.query);
                    apply_filter(app);
                }
                if (input.zr) {  // refresh library from Xbox
                    app.scene = Scene::LoadingLibrary;
                    start_library_load(app, true);
                }
                if (input.minus) {
                    app.auth->logout();
                    std::remove(data_path("games.json").c_str());
                    app.games.clear();
                    app.visible.clear();
                    app.scene = Scene::SignIn;
                    start_signin(app);
                }
                if (input.zl) {
                    app.settings_return = Scene::Library;
                    app.scene = Scene::Settings;
                }
                if (input.a && !app.visible.empty()) {
                    app.launch_game = app.games[app.visible[app.cursor]];
                    push_history(app, app.launch_game.title_id);
#ifdef GNX_NATIVE_STREAM
                    app.engine->start(
                        app.launch_game.title_id,
                        static_cast<QualityTier>(app.settings.quality),
                        kLanguageCodes[app.settings.language],
                        static_cast<stream::VideoPacing>(app.settings.pacing),
                        app.settings.sharpness, app.settings.contrast);
                    app.stream_hint_until = SDL_GetTicks() + 8000;
                    app.scene = Scene::Stream;
#endif
                }
                if (input.plus) running = false;
                break;
            }

            case Scene::Settings: {
                if (input.up)
                    app.settings_cursor = std::max(0, app.settings_cursor - 1);
                if (input.down)
                    app.settings_cursor = std::min(7, app.settings_cursor + 1);
                int direction = (input.right ? 1 : 0) - (input.left ? 1 : 0);
                if (direction != 0) {
                    if (app.settings_cursor == 0)
                        app.settings.quality =
                            (app.settings.quality + direction + 3) % 3;
                    else if (app.settings_cursor == 1)
                        app.settings.pacing =
                            (app.settings.pacing + direction + 2) % 2;
                    else if (app.settings_cursor == 2)
                        app.settings.sharpness =
                            (app.settings.sharpness + direction + 6) % 6;
                    else if (app.settings_cursor == 3)
                        app.settings.contrast =
                            (app.settings.contrast + direction + 5) % 5;
                    else if (app.settings_cursor == 4)
                        app.settings.mapping =
                            (app.settings.mapping + direction + 2) % 2;
                    else if (app.settings_cursor == 5)
                        app.settings.vibration =
                            (app.settings.vibration + direction +
                             kVibrationLevels) %
                            kVibrationLevels;
                    else if (app.settings_cursor == 6) {
                        app.settings.region =
                            (app.settings.region + direction + 6) % 6;
                        apply_region(app.settings);  // takes effect next request
                    } else
                        app.settings.language =
                            (app.settings.language + direction + kLanguageCount) %
                            kLanguageCount;
                    save_settings(app.settings);
                }
                if (input.b || input.zl) app.scene = app.settings_return;
                break;
            }

#ifdef GNX_NATIVE_STREAM
            case Scene::Stream: {
                stream::EngineState stream_state = app.engine->state();
                bool streaming =
                    stream_state == stream::EngineState::Streaming;

                if (streaming) {
                    // Exit combo: - and + held together.
                    bool minus_held =
                        joystick && SDL_JoystickGetButton(joystick, kBtnMinus);
                    bool plus_held =
                        joystick && SDL_JoystickGetButton(joystick, kBtnPlus);
                    if (minus_held && plus_held) {
                        app.engine->stop();
                        app.scene = Scene::Library;
                    }
                } else if (stream_state == stream::EngineState::Failed) {
                    if (input.b) {
                        app.engine->stop();
                        app.scene = Scene::Library;
                    }
                    if (input.a) {  // retry
                        app.engine->start(
                            app.launch_game.title_id,
                            static_cast<QualityTier>(app.settings.quality),
                            kLanguageCodes[app.settings.language],
                            static_cast<stream::VideoPacing>(
                                app.settings.pacing),
                            app.settings.sharpness, app.settings.contrast);
                        app.stream_hint_until = SDL_GetTicks() + 8000;
                    }
                } else if (input.b) {  // cancel while connecting
                    app.engine->stop();
                    app.scene = Scene::Library;
                }
                break;
            }
#else
            case Scene::Stream:
                app.scene = Scene::Library;
                break;
#endif

            case Scene::Fatal:
                if (input.zl) {  // enable Region bypass, then X to retry
                    app.settings_return = Scene::Fatal;
                    app.scene = Scene::Settings;
                    break;
                }
                if (input.x) {
                    app.scene = Scene::LoadingLibrary;
                    start_library_load(app, true);
                }
                if (input.minus) {
                    app.auth->logout();
                    app.scene = Scene::SignIn;
                    start_signin(app);
                }
                if (input.plus || input.b) running = false;
                break;
        }

#ifdef GNX_NATIVE_STREAM
        // Hand the single Switch display between SDL (menus/status) and deko3d
        // (zero-copy video). We switch to deko3d once the first frame is ready
        // and switch back when the streaming phase ends.
        bool want_deko =
            app.scene == Scene::Stream &&
            app.engine->state() == stream::EngineState::Streaming;
        if (want_deko && !app.deko_active) {
            app.covers->drop_textures();  // textures die with the renderer
            app.gfx.suspend();
            if (app.engine->begin_deko_output()) {
                app.deko_active = true;
            } else {
                app.gfx.resume();  // deko3d unavailable -> stay on SDL
            }
        }
        if (!want_deko && app.deko_active) {
            app.engine->end_deko_output();  // release the swapchain first
            app.gfx.resume();
            app.deko_active = false;
#ifdef __SWITCH__
            app.rumble.stop();  // motors off when the stream ends
#endif
        }
        if (app.deko_active) {
            // pump_video decodes everything queued and presents the freshest
            // frame on its own ~60 Hz software clock (it does NOT block on the
            // GPU/vsync -- deko3d's waitIdle only waits for the GPU, and blocking
            // on acquireImage instead crashed). So this loop must run fast and
            // yield 1 ms per spin, or it busy-waits at 100% CPU. Presentation
            // pacing lives entirely in pump_video, decoupled from this loop rate.
            app.engine->pump_video();
            // Pace input at ~125 Hz. The loop spins far faster than the video
            // rate; sending a gamepad packet every spin floods the SCTP input
            // channel ("sctp sendv error 11").
            Uint32 now = SDL_GetTicks();
            if (now - app.last_input_ms >= 8) {
                app.engine->send_gamepad(
                    read_gamepad(joystick, app.settings.mapping));
                apply_rumble(app);
                app.last_input_ms = now;
            }
            SDL_Delay(1);  // yield between spins; present cadence is timer-driven
            continue;      // deko3d owns the frame; no SDL pass this iteration
        }
#endif

        app.covers->pump();
        app.gfx.begin_frame();
        switch (app.scene) {
            case Scene::Splash: draw_splash(app); break;
            case Scene::SignIn: draw_signin(app); break;
            case Scene::LoadingLibrary: draw_loading(app); break;
            case Scene::Library: draw_library(app); break;
            case Scene::Settings: draw_settings(app); break;
            case Scene::Stream:
#ifdef GNX_NATIVE_STREAM
                draw_stream(app, joystick);
#endif
                break;
            case Scene::Fatal: draw_fatal(app); break;
        }
        app.gfx.end_frame();
    }

    // Exit breadcrumbs: if the screen goes black on exit, this file shows the
    // last step reached (hang) or "done" (clean exit -> black is the title-
    // takeover launch artifact, press HOME).
    auto breadcrumb = [](const char* step) {
#ifdef __SWITCH__
        FILE* f = std::fopen("sdmc:/switch/green-nx/exit-log.txt", "a");
        if (f) { std::fprintf(f, "%s\n", step); std::fclose(f); }
#else
        (void)step;
#endif
    };
    breadcrumb("--- exit begin");

    if (app.signin_state == 0) app.signin_state = 4;
    app.abort_http = true;  // unblock any in-flight worker HTTP call
    join_worker(app);
    breadcrumb("workers joined");

#ifdef GNX_NATIVE_STREAM
    app.engine.reset();
    breadcrumb("engine stopped");
#endif
    app.covers.reset();
    breadcrumb("covers stopped");
    app.gfx.shutdown();
    breadcrumb("gfx shut down");
#ifdef __SWITCH__
    romfsExit();
    plExit();
    socketExit();
#endif
    breadcrumb("done");
    return 0;
}
