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
    Splash, SignIn, LoadingLibrary, Library, Detail, Settings, Stream, Fatal
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
    int mapping = 0;    // 0=positional, 1=match labels
    int vibration = 2;  // rumble intensity: 0=Off, 1=Low, 2=Medium, 3=High
    int region = 0;     // region-bypass IP: 0=Off, else index into kRegion*
    int language = 0;   // index into kLanguage* (0 = English US)
    int source = 0;     // 0=xCloud, 1=your Xbox (only offered with a console)
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
    int detail_index = -1;   // games[] index shown in Scene::Detail
    int detail_cursor = 0;   // 0 = Play, 1 = favorite, 2 = Play on... (if any)
    std::vector<HomeConsole> consoles;  // linked Xboxes; empty = hide feature
    bool launching_home = false;        // what the current stream targets

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
    settings.source = std::clamp(data.value("source", 0), 0, 1);
    return settings;
}

void save_settings(const Settings& settings) {
    std::ofstream out(data_path("settings.json"), std::ios::trunc);
    out << json{{"quality", settings.quality},
                {"mapping", settings.mapping},
                {"vibration", settings.vibration},
                {"region", settings.region},
                {"language", settings.language},
                {"source", settings.source}}.dump(2);
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

// Linked consoles are cached like the games list, so the source picker is
// available on cache-served boots too; refreshed on every full library load.
void save_consoles_cache(const std::vector<HomeConsole>& consoles) {
    json list = json::array();
    for (const HomeConsole& console : consoles)
        list.push_back({{"serverId", console.server_id},
                        {"name", console.name},
                        {"consoleType", console.console_type},
                        {"powerState", console.power_state}});
    std::ofstream out(data_path("consoles.json"), std::ios::trunc);
    out << json{{"consoles", list}}.dump();
}

std::vector<HomeConsole> load_consoles_cache() {
    std::vector<HomeConsole> consoles;
    std::ifstream in(data_path("consoles.json"));
    if (!in) return consoles;
    json data = json::parse(in, nullptr, false);
    if (data.is_discarded() || !data.is_object()) return consoles;
    for (const json& entry : data.value("consoles", json::array())) {
        HomeConsole console;
        console.server_id = entry.value("serverId", "");
        console.name = entry.value("name", "");
        console.console_type = entry.value("consoleType", "");
        console.power_state = entry.value("powerState", "");
        if (!console.server_id.empty()) consoles.push_back(std::move(console));
    }
    return consoles;
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
                    app.consoles = load_consoles_cache();
                    app.load_state = 1;
                    return;
                }
            }
            app.status = "Fetching streaming credentials...";
            StreamingCredentials credentials =
                app.auth->fetch_streaming_credentials();
            app.status = "Loading your library...";
            Http http;
            // Linked consoles for xHome remote play. Non-fatal: no consoles
            // (or an error here) just leaves the source picker hidden.
            try {
                app.consoles = fetch_home_consoles(http, credentials.home);
                save_consoles_cache(app.consoles);
            } catch (const std::exception&) {
                app.consoles.clear();
            }
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

// ---- scenes (layout per docs-design/green-nx-redesign.dc.html) ------------

constexpr int kMargin = 60;    // TV-safe margin on all edges
constexpr int kFooterH = 84;
constexpr int kFooterY = gfx::kHeight - kFooterH;

// One footer hint: a button chip plus its label. The screen's primary action
// gets the solid-accent chip.
struct Hint {
    const char* key;
    const char* label;
    bool primary = false;
};

// Chip = 40x40 fill (wider for multi-char keys) + 2px frame + centered glyph.
int chip_width(App& app, const std::string& key) {
    int tw = app.gfx.text_width(key, gfx::FontSize::Small);
    return std::max(40, tw + 16);
}

void draw_chip(App& app, const std::string& key, int x, int y, bool primary) {
    SDL_Rect box = {x, y, chip_width(app, key), 40};
    app.gfx.fill(box, primary ? gfx::kAccent : gfx::kChip);
    if (!primary) app.gfx.frame(box, gfx::kChipEdge, 2);
    app.gfx.text_centered(key, box.x + box.w / 2, y + 4, gfx::FontSize::Small,
                          primary ? gfx::kText : gfx::kText);
}

// Right-aligned hint row. with_bar draws the footer band behind it; screens
// over video (stream) keep the background clear.
void draw_hints(App& app, const std::vector<Hint>& hints,
                bool with_bar = true) {
    if (with_bar) {
        app.gfx.fill({0, kFooterY, gfx::kWidth, kFooterH}, gfx::kBar);
        app.gfx.fill({0, kFooterY, gfx::kWidth, 2}, gfx::kChip);
    }
    int x = gfx::kWidth - kMargin;
    for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
        int lw = app.gfx.text_width(it->label, gfx::FontSize::Small);
        int cw = chip_width(app, it->key);
        x -= cw + 12 + lw;
        int cy = kFooterY + (kFooterH - 40) / 2;
        draw_chip(app, it->key, x, cy, it->primary);
        app.gfx.text(it->label, x + cw + 12, cy + 4, gfx::FontSize::Small,
                     it->primary ? gfx::kText : gfx::kTextDim);
        x -= 32;
    }
}

// Focus system, layer 2+3 of card 1a: 6px kFocus border + two concentric
// "glow" frames (6px at +6 with 40% alpha, 8px at +14 with 15% alpha).
void draw_focus_frames(App& app, const SDL_Rect& r) {
    app.gfx.frame(r, gfx::kFocus, 6);
    app.gfx.frame({r.x - 6, r.y - 6, r.w + 12, r.h + 12},
                  {gfx::kFocus.r, gfx::kFocus.g, gfx::kFocus.b, 102}, 6);
    app.gfx.frame({r.x - 14, r.y - 14, r.w + 28, r.h + 28},
                  {gfx::kFocus.r, gfx::kFocus.g, gfx::kFocus.b, 38}, 8);
}

// Header: 44x44 accent square with "nx" + wordmark, top-left inside margins.
void draw_header(App& app) {
    SDL_Rect logo = {kMargin, 48, 44, 44};
    app.gfx.fill(logo, gfx::kAccent);
    app.gfx.text_centered("nx", logo.x + 22, logo.y + 6, gfx::FontSize::Small,
                          gfx::kText);
    app.gfx.text("green-nx", logo.x + 64, logo.y + 4, gfx::FontSize::Note,
                 gfx::kText);
}

// Fallback cover: per-title dark hue + big translucent initials, so a grid
// with covers still downloading reads as content instead of gray boxes.
const gfx::Color kCoverHues[12] = {
    {35, 48, 71},  {58, 36, 48},  {31, 58, 51},  {59, 50, 32},
    {44, 36, 64},  {64, 38, 32},  {32, 48, 63},  {51, 32, 44},
    {36, 49, 58},  {49, 42, 30},  {30, 44, 36},  {46, 34, 51}};

gfx::Color cover_hue(const std::string& title_id) {
    unsigned hash = 5381;
    for (unsigned char c : title_id) hash = hash * 33 + c;
    return kCoverHues[hash % 12];
}

std::string cover_initials(const Game& game) {
    const std::string& name = game.name.empty() ? game.title_id : game.name;
    std::string initials;
    bool word_start = true;
    for (char c : name) {
        if (c == ' ') { word_start = true; continue; }
        if (word_start && initials.size() < 2)
            initials += static_cast<char>(std::toupper(c));
        word_start = false;
    }
    return initials;
}

void draw_cover_fallback(App& app, const Game& game, const SDL_Rect& rect,
                         gfx::FontSize initial_size) {
    app.gfx.fill(rect, cover_hue(game.title_id));
    app.gfx.text_centered(cover_initials(game), rect.x + rect.w / 2,
                          rect.y + rect.h / 2 - 40, initial_size,
                          {255, 255, 255, 36});
}

void draw_splash(App& app) {
    Uint32 elapsed = SDL_GetTicks() - app.scene_started;
    int logo_w = static_cast<int>(120 * std::min(elapsed / 300.0f, 1.0f));
    int word_w = app.gfx.text_width("green-nx", gfx::FontSize::Huge);
    int row_w = 120 + 36 + word_w;
    int x0 = (gfx::kWidth - row_w) / 2;
    if (logo_w > 0) {
        app.gfx.fill({x0, 400, logo_w, 120}, gfx::kAccent);
        if (logo_w == 120)
            app.gfx.text_centered("nx", x0 + 60, 428, gfx::FontSize::Title,
                                  gfx::kText);
    }
    if (elapsed > 300) {
        app.gfx.text("green-nx", x0 + 156, 408, gfx::FontSize::Huge,
                     gfx::kText);
        app.gfx.text_centered("Xbox Cloud Gaming for Nintendo Switch",
                              gfx::kWidth / 2, 548, gfx::FontSize::Note,
                              gfx::kTextDim);
    }
    // The bar IS the boot indicator: no spinner (card 1b).
    SDL_Rect track = {gfx::kWidth / 2 - 180, 648, 360, 6};
    app.gfx.fill(track, gfx::kChip);
    int progress = static_cast<int>(360 * std::min(elapsed / 1200.0f, 1.0f));
    app.gfx.fill({track.x, track.y, progress, 6}, gfx::kFocus);
}

// Numbered step bullet used by the sign-in screen.
void draw_step_box(App& app, const char* number, int x, int y) {
    SDL_Rect box = {x, y, 44, 44};
    app.gfx.fill(box, gfx::kSurface);
    app.gfx.frame(box, gfx::kChipEdge, 2);
    app.gfx.text_centered(number, x + 22, y + 6, gfx::FontSize::Small,
                          gfx::kTextDim);
}

// The device code split in two groups of glyphs drawn with a fixed 14px
// extra advance (readable at 3 m, card 1c).
int spaced_code_width(App& app, const std::string& group) {
    int w = 0;
    for (char c : group)
        w += app.gfx.text_width(std::string(1, c), gfx::FontSize::Huge) + 14;
    return w > 0 ? w - 14 : 0;
}

int draw_spaced_code(App& app, const std::string& group, int x, int y) {
    for (char c : group) {
        x += app.gfx.text(std::string(1, c), x, y, gfx::FontSize::Huge,
                          gfx::kText) + 14;
    }
    return x;
}

void draw_signin(App& app) {
    draw_header(app);
    app.gfx.text_centered("Sign in with Microsoft", gfx::kWidth / 2, 170,
                          gfx::FontSize::Title, gfx::kText);
    if (app.device_code.user_code.empty()) {
        app.gfx.spinner(gfx::kWidth / 2, 480, SDL_GetTicks());
        draw_hints(app, {{"B", "Exit"}});
        return;
    }

    // Step 1: box + text + URL chip, centered as one row.
    const std::string& uri = app.device_code.verification_uri;
    int uri_w = app.gfx.text_width(uri, gfx::FontSize::Body) + 56;
    int t1_w = app.gfx.text_width("On your phone or computer, open",
                                  gfx::FontSize::Note);
    int row1_w = 44 + 24 + t1_w + 24 + uri_w;
    int x = (gfx::kWidth - row1_w) / 2;
    draw_step_box(app, "1", x, 300);
    app.gfx.text("On your phone or computer, open", x + 68, 306,
                 gfx::FontSize::Note, gfx::kTextDim);
    SDL_Rect uri_box = {x + 68 + t1_w + 24, 292, uri_w, 62};
    app.gfx.fill(uri_box, gfx::kSurface);
    app.gfx.text_centered(uri, uri_box.x + uri_box.w / 2, 300,
                          gfx::FontSize::Body, gfx::kFocus);

    // Step 2.
    int t2_w = app.gfx.text_width("and enter this code", gfx::FontSize::Note);
    x = (gfx::kWidth - (44 + 24 + t2_w)) / 2;
    draw_step_box(app, "2", x, 392);
    app.gfx.text("and enter this code", x + 68, 398, gfx::FontSize::Note,
                 gfx::kTextDim);

    // The code, split 4+4 (or at the dash) to avoid read errors.
    std::string code = app.device_code.user_code;
    std::string left = code, right;
    size_t dash = code.find('-');
    if (dash != std::string::npos) {
        left = code.substr(0, dash);
        right = code.substr(dash + 1);
    } else if (code.size() > 4) {
        left = code.substr(0, code.size() / 2);
        right = code.substr(code.size() / 2);
    }
    int lw = spaced_code_width(app, left);
    int rw = spaced_code_width(app, right);
    int inner = lw + (right.empty() ? 0 : 48 + 24 + 48 + rw);
    SDL_Rect box = {(gfx::kWidth - (inner + 160)) / 2, 480, inner + 160, 190};
    app.gfx.fill(box, gfx::kSurface);
    app.gfx.frame(box, gfx::kAccent, 4);
    int cx = box.x + 80;
    cx = draw_spaced_code(app, left, cx, box.y + 34);
    if (!right.empty()) {
        app.gfx.fill({cx + 48, box.y + box.h / 2 - 4, 24, 8}, gfx::kChipEdge);
        draw_spaced_code(app, right, cx + 48 + 24 + 48, box.y + 34);
    }

    app.gfx.spinner(gfx::kWidth / 2 - 140, 736, SDL_GetTicks());
    app.gfx.text("Waiting for you to sign in…", gfx::kWidth / 2 - 90, 728,
                 gfx::FontSize::Small, gfx::kTextDim);
    draw_hints(app, {{"ZL", "Settings · region bypass"}, {"B", "Exit"}});
}

// Grid geometry (card 1e): 6 columns of 230x345 covers from (170,220),
// 40px column gap, 72px row gap (room for the focused card's name plate).
constexpr int kColumns = 6;
constexpr int kCardW = 230;
constexpr int kCardH = 345;
constexpr int kGapX = 40;
constexpr int kGapY = 72;
constexpr int kGridX = 170;
constexpr int kGridY = 220;
constexpr int kRowsVisible = 2;

const char* kTabNames[kTabCount] = {"All games", "Favorites", "History"};

const char* kQualityLabels[3] = {"720p", "1080p", "1080p high bitrate"};
const char* kMappingLabels[2] = {"Positional (Switch A = Xbox B)",
                                 "Match labels (Switch A = Xbox A)"};

// Skeleton shades: each card one step darker, hinting at content loading in.
const gfx::Color kSkeleton[6] = {{22, 27, 36}, {20, 24, 33}, {18, 21, 29},
                                 {16, 19, 25}, {14, 17, 22}, {13, 15, 20}};

void draw_loading(App& app) {
    draw_header(app);
    app.gfx.fill({kGridX, 124, 220, 38}, gfx::kSurface);
    for (int i = 0; i < 6; ++i)
        app.gfx.fill({kGridX + i * (kCardW + kGapX), kGridY, kCardW, kCardH},
                     kSkeleton[i]);
    app.gfx.spinner(gfx::kWidth / 2, 700, SDL_GetTicks());
    app.gfx.text_centered(app.status, gfx::kWidth / 2, 744,
                          gfx::FontSize::Note, gfx::kTextDim);
}

// One library card. The focused card scales 1.08x (230x345 -> 248x372,
// centered), gets border+glow and a name plate under it (card 1a layer 1+4).
void draw_card(App& app, const Game& game, const SDL_Rect& card,
               bool focused) {
    SDL_Rect dst = card;
    if (focused) dst = {card.x - 9, card.y - 13, kCardW + 18, kCardH + 27};

    SDL_Texture* cover = app.covers->get(game.title_id, game.box_art_url);
    if (cover)
        app.gfx.draw_texture(cover, dst);
    else
        draw_cover_fallback(app, game, dst, gfx::FontSize::Huge);

    if (is_favorite(app, game.title_id)) {
        SDL_Rect badge = {dst.x + 8, dst.y + 8, 44, 44};
        app.gfx.fill(badge, gfx::kWarn);
        app.gfx.text_centered("★", badge.x + 22, badge.y + 6,
                              gfx::FontSize::Small, gfx::kBg);
    }

    if (focused) {
        draw_focus_frames(app, dst);
        SDL_Rect plate = {card.x - 18, card.y + kCardH + 14, kCardW + 36, 44};
        app.gfx.fill(plate, gfx::kSurface);
        const std::string& label =
            game.name.empty() ? game.title_id : game.name;
        app.gfx.text_centered(label.substr(0, 24), plate.x + plate.w / 2,
                              plate.y + 8, gfx::FontSize::Small, gfx::kText);
    }
}

// Empty-state pattern (card 1l): big glyph box + title + instruction with
// the relevant button chip embedded in the line.
void draw_empty_state(App& app, const std::string& glyph, gfx::Color glyph_col,
                      const std::string& title, const std::string& pre,
                      const char* chip_key, const std::string& post) {
    if (!glyph.empty()) {
        SDL_Rect box = {gfx::kWidth / 2 - 60, 380, 120, 120};
        app.gfx.fill(box, gfx::kSurface);
        app.gfx.text_centered(glyph, box.x + 60, box.y + 28,
                              gfx::FontSize::Title, glyph_col);
    }
    app.gfx.text_centered(title, gfx::kWidth / 2, 540, gfx::FontSize::Body,
                          gfx::kText);
    int pre_w = app.gfx.text_width(pre, gfx::FontSize::Note);
    int post_w = app.gfx.text_width(post, gfx::FontSize::Note);
    int cw = chip_key ? chip_width(app, chip_key) : 0;
    int total = pre_w + (chip_key ? 14 + cw + 14 : 0) + post_w;
    int x = (gfx::kWidth - total) / 2;
    x += app.gfx.text(pre, x, 616, gfx::FontSize::Note, gfx::kTextDim);
    if (chip_key) {
        draw_chip(app, chip_key, x + 14, 612, false);
        x += 14 + cw + 14;
    }
    app.gfx.text(post, x, 616, gfx::FontSize::Note, gfx::kTextDim);
}

void draw_library(App& app) {
    // Row 1: identity (logo left, gamertag + source chip right). The source
    // chip only exists when a console is linked (card 1e visibility rule).
    draw_header(app);
    int right = gfx::kWidth - kMargin;
    if (!app.gamertag.empty()) {
        int gt_w = app.gfx.text_width(app.gamertag, gfx::FontSize::Small);
        app.gfx.text(app.gamertag, right - gt_w, 56, gfx::FontSize::Small,
                     gfx::kTextDim);
        right -= gt_w + 28;
    }
    if (!app.consoles.empty()) {
        bool home = app.settings.source == 1;
        std::string label =
            home ? (app.consoles[0].name.empty() ? "Your Xbox"
                                                 : app.consoles[0].name)
                 : std::string("xCloud · ") +
                       kQualityLabels[app.settings.quality];
        int lw = app.gfx.text_width(label, gfx::FontSize::Small);
        SDL_Rect chip = {right - (lw + 64), 48, lw + 64, 44};
        app.gfx.fill(chip, gfx::kSurface);
        app.gfx.fill({chip.x + 20, chip.y + 16, 12, 12}, gfx::kFocus);
        app.gfx.text(label, chip.x + 44, chip.y + 6, gfx::FontSize::Small,
                     gfx::kText);
    }

    // Row 2: navigation — L/R chips hugging the tabs (the hint lives where it
    // acts), active tab kText + 5px accent underline, idle tabs kFaint.
    int tx = kGridX;
    draw_chip(app, "L", tx, 128, false);
    tx += chip_width(app, "L") + 44;
    for (int t = 0; t < kTabCount; ++t) {
        bool active = static_cast<int>(app.tab) == t;
        int w = app.gfx.text(kTabNames[t], tx, 124, gfx::FontSize::Body,
                             active ? gfx::kText : gfx::kFaint);
        if (active) app.gfx.fill({tx, 176, w, 5}, gfx::kAccent);
        tx += w + 44;
    }
    draw_chip(app, "R", tx, 128, false);

    std::string info = std::to_string(app.visible.size()) + " games";
    if (!app.query.empty()) info += "  ·  \"" + app.query + "\"";
    app.gfx.text(info,
                 gfx::kWidth - kGridX -
                     app.gfx.text_width(info, gfx::FontSize::Small),
                 138, gfx::FontSize::Small, gfx::kFaint);

    if (app.visible.empty()) {
        if (!app.query.empty())
            draw_empty_state(app, "", gfx::kText,
                             "Nothing found for \"" + app.query + "\"",
                             "Press", "Y", "to search again");
        else if (app.tab == LibraryTab::Favorites)
            draw_empty_state(app, "★", gfx::kWarn, "No favorites yet",
                             "Press", "X", "on any game to pin it here");
        else if (app.tab == LibraryTab::History)
            draw_empty_state(app, "…", gfx::kTextDim, "Nothing played yet",
                             "Games you launch appear here", nullptr, "");
        else
            draw_empty_state(app, "", gfx::kText,
                             "No games available for this account",
                             "Press", "ZR", "to refresh your library");
    }

    // Draw the focused card last so its scale/glow overlaps neighbours.
    int first_row = std::max(0, app.cursor / kColumns - (kRowsVisible - 1));
    int focused_slot = -1;
    for (int slot = 0; slot < kColumns * (kRowsVisible + 1); ++slot) {
        int index = first_row * kColumns + slot;
        if (index >= static_cast<int>(app.visible.size())) break;
        int column = slot % kColumns;
        int row = slot / kColumns;
        SDL_Rect card = {kGridX + column * (kCardW + kGapX),
                         kGridY + row * (kCardH + kGapY), kCardW, kCardH};
        if (card.y + 40 > kFooterY) break;
        if (index == app.cursor) {
            focused_slot = slot;
            continue;
        }
        draw_card(app, app.games[app.visible[index]], card, false);
    }
    if (focused_slot >= 0) {
        int column = focused_slot % kColumns;
        int row = focused_slot / kColumns;
        SDL_Rect card = {kGridX + column * (kCardW + kGapX),
                         kGridY + row * (kCardH + kGapY), kCardW, kCardH};
        draw_card(app, app.games[app.visible[app.cursor]], card, true);
    }

    draw_hints(app, {{"A", "Details", true},
                     {"X", "Favorite"},
                     {"Y", "Search"},
                     {"ZR", "Refresh"},
                     {"ZL", "Settings"},
                     {"−", "Sign out"},
                     {"+", "Exit"}});
}

// Game detail (card 1f): big cover left, title + meta chips + action buttons
// right. Play is focused on entry, so A-A launches as fast as before.
void draw_detail(App& app) {
    if (app.detail_index < 0 ||
        app.detail_index >= static_cast<int>(app.games.size()))
        return;
    const Game& game = app.games[app.detail_index];

    SDL_Rect cover_rect = {kGridX, 120, 520, 780};
    SDL_Texture* cover = app.covers->get(game.title_id, game.box_art_url);
    if (cover)
        app.gfx.draw_texture(cover, cover_rect);
    else
        draw_cover_fallback(app, game, cover_rect, gfx::FontSize::Huge);
    bool fav = is_favorite(app, game.title_id);
    if (fav) {
        SDL_Rect badge = {cover_rect.x + 12, cover_rect.y + 12, 56, 56};
        app.gfx.fill(badge, gfx::kWarn);
        app.gfx.text_centered("★", badge.x + 28, badge.y + 10,
                              gfx::FontSize::Body, gfx::kBg);
    }

    int rx = 790;
    const std::string& title = game.name.empty() ? game.title_id : game.name;
    app.gfx.text(title.substr(0, 34), rx, 150, gfx::FontSize::Title,
                 gfx::kText);

    // Meta chips: kSurface pills with XS text.
    int cx2 = rx;
    auto meta_chip = [&](const std::string& label, gfx::Color color) {
        int w = app.gfx.text_width(label, gfx::FontSize::Small) + 36;
        app.gfx.fill({cx2, 232, w, 44}, gfx::kSurface);
        app.gfx.text(label, cx2 + 18, 238, gfx::FontSize::Small, color);
        cx2 += w + 20;
    };
    meta_chip("Xbox Cloud Gaming", gfx::kTextDim);
    meta_chip(kQualityLabels[app.settings.quality], gfx::kTextDim);
    if (fav) meta_chip("★ Favorite", gfx::kWarn);

    // Action buttons, 640 wide. Buttons don't scale on focus — only
    // border+glow (and the primary keeps its accent fill). "Play on…" is
    // drawn only with a linked console (card 1f visibility rule).
    const char* fav_label = fav ? "★ Remove favorite" : "★ Add favorite";
    SDL_Rect play = {rx, 400, 640, 96};
    app.gfx.fill(play, gfx::kAccent);
    app.gfx.text_centered("Play", play.x + 320, play.y + 24,
                          gfx::FontSize::Body, gfx::kText);
    SDL_Rect favbtn = {rx, 520, 640, 96};
    app.gfx.fill(favbtn, gfx::kSurface);
    app.gfx.text_centered(fav_label, favbtn.x + 320, favbtn.y + 24,
                          gfx::FontSize::Body, gfx::kText);
    SDL_Rect source = {rx, 640, 640, 96};
    if (!app.consoles.empty()) {
        bool home = app.settings.source == 1;
        app.gfx.fill(source, gfx::kSurface);
        app.gfx.text("Play on…", source.x + 44, source.y + 24,
                     gfx::FontSize::Body, gfx::kText);
        std::string target =
            home ? (app.consoles[0].name.empty() ? "Your Xbox"
                                                 : app.consoles[0].name)
                 : "xCloud";
        app.gfx.text(target,
                     source.x + source.w - 44 -
                         app.gfx.text_width(target, gfx::FontSize::Body),
                     source.y + 24, gfx::FontSize::Body, gfx::kTextDim);
    }
    SDL_Rect focused = app.detail_cursor == 0   ? play
                       : app.detail_cursor == 1 ? favbtn
                                                : source;
    draw_focus_frames(app, focused);

    app.gfx.text("Streams in your account's language · change in Settings",
                 rx, app.consoles.empty() ? 680 : 792, gfx::FontSize::Small,
                 gfx::kFaint);

    draw_hints(app, {{"A", "Select", true}, {"B", "Back"}});
}

void draw_settings(App& app) {
    app.gfx.text("Settings", kMargin, 48, gfx::FontSize::Title, gfx::kText);

    struct Row {
        const char* title;
        std::string value;
    };
    std::vector<Row> rows = {
        {"Stream quality", kQualityLabels[app.settings.quality]},
        {"Button layout", kMappingLabels[app.settings.mapping]},
        {"Vibration", kVibrationLabels[app.settings.vibration]},
        {"Region bypass", kRegionLabels[app.settings.region]},
        {"Game language", kLanguageLabels[app.settings.language]},
    };
    if (!app.consoles.empty())
        rows.push_back({"Preferred source",
                        app.settings.source == 1
                            ? (app.consoles[0].name.empty()
                                   ? "Your Xbox"
                                   : app.consoles[0].name)
                            : "xCloud"});
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        SDL_Rect row = {120, 170 + i * 108, gfx::kWidth - 240, 96};
        bool focused = i == app.settings_cursor;
        // Row-focus variant (card 1g): wide elements don't scale — surface
        // lift + 10px side bar + 4px border + one glow frame instead.
        app.gfx.fill(row, focused ? gfx::kSurfaceHi : gfx::kSurface);
        if (focused) {
            app.gfx.fill({row.x, row.y, 10, row.h}, gfx::kFocus);
            app.gfx.frame(row, gfx::kFocus, 4);
            app.gfx.frame({row.x - 4, row.y - 4, row.w + 8, row.h + 8},
                          {gfx::kFocus.r, gfx::kFocus.g, gfx::kFocus.b, 90},
                          5);
        }
        app.gfx.text(rows[i].title, row.x + 68, row.y + 26,
                     gfx::FontSize::Body, gfx::kText);
        int vw = app.gfx.text_width(rows[i].value, gfx::FontSize::Body);
        if (focused) {
            int vx = row.x + row.w - 44 - vw;
            app.gfx.text("‹", vx - 56, row.y + 26, gfx::FontSize::Body,
                         gfx::kTextDim);
            app.gfx.text(rows[i].value, vx, row.y + 26, gfx::FontSize::Body,
                         gfx::kFocus);
            app.gfx.text("›", row.x + row.w - 44 + 20, row.y + 26,
                         gfx::FontSize::Body, gfx::kTextDim);
        } else {
            app.gfx.text(rows[i].value, row.x + row.w - 44 - vw, row.y + 26,
                         gfx::FontSize::Body, gfx::kAccent);
        }
    }

    // Contextual note: a fixed structured box (fill kBar + frame + accent
    // side bar), swapping content with the focused row. The bar turns kWarn
    // while Region bypass is active.
    const char* line1;
    const char* line2;
    switch (app.settings_cursor) {
        case 5:
            line1 = "Where Play launches games: xCloud (cloud servers) or";
            line2 = "remote play from your own console over your network.";
            break;
        case 1:
            line1 = "Positional keeps the Switch layout under your thumbs;";
            line2 = "match labels follows the printed A/B/X/Y letters.";
            break;
        case 2:
            line1 = "Rumble intensity for the game's vibration effects.";
            line2 = "High still leaves headroom to avoid the HD-rumble hum.";
            break;
        case 3:
            line1 = "Region bypass spoofs your location to Xbox to reach";
            line2 = "xCloud from an unsupported country. Use at your own risk.";
            break;
        case 4:
            line1 = "Sets the streamed console's language for games without";
            line2 = "an in-game language menu. Takes effect on next launch.";
            break;
        default:
            line1 = "Higher quality needs a stronger connection — 5 GHz";
            line2 = "Wi-Fi or docked LAN is recommended for high bitrate.";
            break;
    }
    SDL_Rect note = {120, 820, gfx::kWidth - 240, 120};
    app.gfx.fill(note, gfx::kBar);
    app.gfx.frame(note, gfx::kChip, 2);
    app.gfx.fill({note.x + 28, note.y + 28, 8, 64},
                 app.settings.region != 0 ? gfx::kWarn : gfx::kAccent);
    app.gfx.text(line1, note.x + 64, note.y + 20, gfx::FontSize::Note,
                 gfx::kTextDim);
    app.gfx.text(line2, note.x + 64, note.y + 60, gfx::FontSize::Note,
                 gfx::kTextDim);

    draw_hints(app, {{"◀ ▶", "Change"}, {"B", "Back"}});
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

// Shared error card (cards 1i/1j): top 8px error band + a boxed card with
// an "!" glyph header, message body, and optional context/log lines.
// Returns the y where extra content (suggestion box) may continue.
int draw_error_card(App& app, const SDL_Rect& card, const char* title,
                    const std::string& message, const std::string& context,
                    bool show_log_path) {
    app.gfx.fill({0, 0, gfx::kWidth, 8}, gfx::kError);
    app.gfx.fill(card, gfx::kBar);
    app.gfx.frame(card, gfx::kChipEdge, 2);

    SDL_Rect icon = {card.x + 48, card.y + 36, 56, 56};
    app.gfx.fill(icon, {42, 20, 22});
    app.gfx.frame(icon, gfx::kError, 3);
    app.gfx.text_centered("!", icon.x + 28, icon.y + 10, gfx::FontSize::Body,
                          gfx::kError);
    app.gfx.text(title, icon.x + 80, card.y + 34, gfx::FontSize::Title,
                 gfx::kError);
    app.gfx.fill({card.x, card.y + 128, card.w, 2}, gfx::kChip);

    int y = card.y + 164;
    app.gfx.text(message.substr(0, 60), card.x + 48, y, gfx::FontSize::Body,
                 gfx::kText);
    if (message.size() > 60) {
        y += 52;
        app.gfx.text(message.substr(60, 60), card.x + 48, y,
                     gfx::FontSize::Body, gfx::kText);
    }
    y += 60;
    if (!context.empty()) {
        app.gfx.text(context, card.x + 48, y, gfx::FontSize::Note,
                     gfx::kTextDim);
        y += 52;
    }
    if (show_log_path) {
        SDL_Rect log = {card.x + 48, y, card.w - 96, 56};
        app.gfx.fill(log, gfx::kSurface);
        app.gfx.text("log: /switch/green-nx/stream-log.txt", log.x + 24,
                     log.y + 12, gfx::FontSize::Small, gfx::kTextDim);
        y += 76;
    }
    return y;
}

// Map the engine's status line onto the 4 real connection stages shown under
// the progress bar (card 1h).
int stream_phase(const std::string& status) {
    std::string s = lowercase(status);
    if (s.find("video") != std::string::npos ||
        s.find("frame") != std::string::npos)
        return 3;
    if (s.find("dtls") != std::string::npos ||
        s.find("handshake") != std::string::npos ||
        s.find("srtp") != std::string::npos)
        return 2;
    if (s.find("ice") != std::string::npos ||
        s.find("candidate") != std::string::npos ||
        s.find("connect") != std::string::npos)
        return 1;
    return 0;
}

void draw_stream(App& app, SDL_Joystick* joystick) {
    stream::EngineState state = app.engine->state();

    // Pure black behind everything: the video arrives over black, so the
    // SDL->deko3d handoff never flashes a colored frame.
    app.gfx.fill({0, 0, gfx::kWidth, gfx::kHeight}, {0, 0, 0});

    if (state == stream::EngineState::Failed) {
        SDL_Rect card = {460, 250, 1000, 460};
        draw_error_card(app, card, "Stream failed",
                        app.engine->error(),
                        app.launch_game.name.empty() ? app.launch_game.title_id
                                                     : app.launch_game.name,
                        true);
        draw_hints(app, {{"A", "Retry", true}, {"B", "Back to library"}});
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

        if (SDL_GetTicks() < app.stream_hint_until) {
            app.gfx.fill({0, kFooterY, gfx::kWidth, kFooterH},
                         {0, 0, 0, 153});
            app.gfx.text_centered(
                "Hold  −  and  +  together to leave the stream",
                gfx::kWidth / 2, gfx::kHeight - 62, gfx::FontSize::Small,
                gfx::kTextDim);
        }
        return;
    }

    // Connecting (card 1h): the cover the user just chose, honest stage
    // labels for the 4 real engine phases, and a bar that moves per phase.
    SDL_Rect mini = {gfx::kWidth / 2 - 80, 300, 160, 240};
    SDL_Texture* cover =
        app.covers->get(app.launch_game.title_id, app.launch_game.box_art_url);
    if (cover)
        app.gfx.draw_texture(cover, mini);
    else
        draw_cover_fallback(app, app.launch_game, mini, gfx::FontSize::Title);

    const std::string& label =
        app.launch_game.name.empty() ? app.launch_game.title_id
                                     : app.launch_game.name;
    app.gfx.text_centered(label, gfx::kWidth / 2, 584, gfx::FontSize::Title,
                          gfx::kText);
    app.gfx.text_centered(app.engine->status(), gfx::kWidth / 2, 664,
                          gfx::FontSize::Note, gfx::kTextDim);

    int phase = stream_phase(app.engine->status());
    SDL_Rect track = {gfx::kWidth / 2 - 280, 736, 560, 6};
    app.gfx.fill(track, gfx::kChip);
    app.gfx.fill({track.x, track.y, 140 * (phase + 1), 6}, gfx::kFocus);

    const char* stages[4] = {"Session", "ICE", "DTLS", "Video"};
    int total = 0;
    for (int i = 0; i < 4; ++i)
        total += app.gfx.text_width(stages[i], gfx::FontSize::Small) + 32;
    int sx = (gfx::kWidth - (total - 32)) / 2;
    for (int i = 0; i < 4; ++i) {
        gfx::Color color = i < phase ? gfx::kFocus
                           : i == phase ? gfx::kText
                                        : gfx::kFaint;
        sx += app.gfx.text(stages[i], sx, 768, gfx::FontSize::Small, color) +
              32;
    }

    // Source/quality chip, top right.
    std::string quality =
        app.launching_home
            ? (app.consoles.empty() || app.consoles[0].name.empty()
                   ? std::string("Your Xbox")
                   : app.consoles[0].name)
            : std::string("xCloud · ") + kQualityLabels[app.settings.quality];
    int qw = app.gfx.text_width(quality, gfx::FontSize::Small) + 40;
    app.gfx.fill({gfx::kWidth - kMargin - qw, 48, qw, 44},
                 {gfx::kSurface.r, gfx::kSurface.g, gfx::kSurface.b, 217});
    app.gfx.text(quality, gfx::kWidth - kMargin - qw + 20, 54,
                 gfx::FontSize::Small, gfx::kTextDim);

    draw_hints(app, {{"B", "Cancel"}}, /*with_bar=*/false);
}
#endif

void draw_fatal(App& app) {
    SDL_Rect card = {410, 180, 1100,
                     app.fatal.find("streaming login") != std::string::npos
                         ? 560
                         : 400};
    int y = draw_error_card(app, card, "Something went wrong", app.fatal, "",
                            false);
    // The streaming-login step is the geo gate; if it failed, point the user
    // at Region bypass instead of leaving a dead end (card 1j).
    if (app.fatal.find("streaming login") != std::string::npos) {
        SDL_Rect tip = {card.x + 48, y, card.w - 96, 150};
        app.gfx.fill(tip, gfx::kSurface);
        app.gfx.frame(tip, gfx::kWarn, 2);
        app.gfx.fill({tip.x + 28, tip.y + 28, 8, 94}, gfx::kWarn);
        app.gfx.text("Xbox Cloud Gaming may be unavailable in your region.",
                     tip.x + 64, tip.y + 22, gfx::FontSize::Note, gfx::kText);
        app.gfx.text("Press ZL for Settings, turn on Region bypass, then X.",
                     tip.x + 64, tip.y + 74, gfx::FontSize::Note,
                     gfx::kTextDim);
    }
    draw_hints(app, {{"X", "Retry", true},
                     {"ZL", "Settings"},
                     {"−", "Sign out"},
                     {"+", "Exit"}});
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
                    // Card 1f: A opens the detail screen with Play focused,
                    // so A-A still launches as fast as direct launch did.
                    app.detail_index = app.visible[app.cursor];
                    app.detail_cursor = 0;
                    app.scene = Scene::Detail;
                }
                if (input.plus) running = false;
                break;
            }

            case Scene::Detail: {
                int last = app.consoles.empty() ? 1 : 2;
                if (input.up)
                    app.detail_cursor = std::max(0, app.detail_cursor - 1);
                if (input.down)
                    app.detail_cursor = std::min(last, app.detail_cursor + 1);
                if (input.b) app.scene = Scene::Library;
                if (input.a && app.detail_index >= 0 &&
                    app.detail_index < static_cast<int>(app.games.size())) {
                    const Game& game = app.games[app.detail_index];
                    if (app.detail_cursor == 1) {
                        toggle_favorite(app, game.title_id);
                        apply_filter(app);
                    } else if (app.detail_cursor == 2) {
                        // Toggle the preferred source; remembered for every
                        // future launch until changed here or in Settings.
                        app.settings.source = app.settings.source == 1 ? 0 : 1;
                        save_settings(app.settings);
                    } else {
                        app.launch_game = game;
                        app.launching_home =
                            app.settings.source == 1 && !app.consoles.empty();
                        push_history(app, app.launch_game.title_id);
#ifdef GNX_NATIVE_STREAM
                        if (app.launching_home)
                            app.engine->start_home(
                                app.consoles[0].server_id,
                                static_cast<QualityTier>(app.settings.quality),
                                kLanguageCodes[app.settings.language]);
                        else
                            app.engine->start(
                                app.launch_game.title_id,
                                static_cast<QualityTier>(app.settings.quality),
                                kLanguageCodes[app.settings.language]);
                        app.stream_hint_until = SDL_GetTicks() + 8000;
                        app.scene = Scene::Stream;
#endif
                    }
                }
                break;
            }

            case Scene::Settings: {
                int last_row = app.consoles.empty() ? 4 : 5;
                if (input.up)
                    app.settings_cursor = std::max(0, app.settings_cursor - 1);
                if (input.down)
                    app.settings_cursor =
                        std::min(last_row, app.settings_cursor + 1);
                int direction = (input.right ? 1 : 0) - (input.left ? 1 : 0);
                if (direction != 0) {
                    if (app.settings_cursor == 0)
                        app.settings.quality =
                            (app.settings.quality + direction + 3) % 3;
                    else if (app.settings_cursor == 1)
                        app.settings.mapping =
                            (app.settings.mapping + direction + 2) % 2;
                    else if (app.settings_cursor == 2)
                        app.settings.vibration =
                            (app.settings.vibration + direction +
                             kVibrationLevels) %
                            kVibrationLevels;
                    else if (app.settings_cursor == 3) {
                        app.settings.region =
                            (app.settings.region + direction + 6) % 6;
                        apply_region(app.settings);  // takes effect next request
                    } else if (app.settings_cursor == 4)
                        app.settings.language =
                            (app.settings.language + direction + kLanguageCount) %
                            kLanguageCount;
                    else
                        app.settings.source = app.settings.source == 1 ? 0 : 1;
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
                    if (input.a) {  // retry the same target
                        if (app.launching_home && !app.consoles.empty())
                            app.engine->start_home(
                                app.consoles[0].server_id,
                                static_cast<QualityTier>(app.settings.quality),
                                kLanguageCodes[app.settings.language]);
                        else
                            app.engine->start(
                                app.launch_game.title_id,
                                static_cast<QualityTier>(app.settings.quality),
                                kLanguageCodes[app.settings.language]);
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
            case Scene::Detail: draw_detail(app); break;
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
