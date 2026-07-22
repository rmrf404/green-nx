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

struct Settings {
    int quality = 2;  // 0=720p, 1=1080p, 2=1080p HQ
    int mapping = 0;  // 0=positional, 1=match labels
};

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

    // library
    std::vector<Game> games;
    std::vector<int> visible;  // indices into games after search filter
    std::string query;
    int cursor = 0;
    std::atomic<int> load_state{0};  // 0 running, 1 ok, 2 error
    std::string load_error;
    std::string gamertag;
    Game launch_game;
    Settings settings;
    int settings_cursor = 0;

#ifdef GNX_NATIVE_STREAM
    std::unique_ptr<stream::Engine> engine;
    Uint32 stream_hint_until = 0;
    bool deko_active = false;  // deko3d owns the display (SDL suspended)
    Uint32 last_input_ms = 0;  // input pacing during deko3d streaming
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
    return settings;
}

void save_settings(const Settings& settings) {
    std::ofstream out(data_path("settings.json"), std::ios::trunc);
    out << json{{"quality", settings.quality},
                {"mapping", settings.mapping}}.dump(2);
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
            SDL_Delay(static_cast<Uint32>(
                std::max(app.device_code.interval_secs, 1) * 1000));
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

void apply_filter(App& app) {
    app.visible.clear();
    std::string needle = lowercase(app.query);
    for (int i = 0; i < static_cast<int>(app.games.size()); ++i) {
        if (needle.empty() ||
            lowercase(app.games[i].name).find(needle) != std::string::npos ||
            lowercase(app.games[i].title_id).find(needle) !=
                std::string::npos)
            app.visible.push_back(i);
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
    draw_footer(app, "Waiting for sign-in...   B  Exit");
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

void draw_library(App& app) {
    // Header
    app.gfx.text("green-nx", 60, 40, gfx::FontSize::Title, gfx::kText);
    std::string counter =
        std::to_string(app.visible.size()) + " games" +
        (app.query.empty() ? "" : "  ·  search: \"" + app.query + "\"");
    app.gfx.text(counter, 60, 105, gfx::FontSize::Small, gfx::kTextDim);
    if (!app.gamertag.empty())
        app.gfx.text(app.gamertag,
                     gfx::kWidth - 60 -
                         app.gfx.text_width(app.gamertag,
                                            gfx::FontSize::Body),
                     55, gfx::FontSize::Body, gfx::kTextDim);

    if (app.visible.empty()) {
        app.gfx.text_centered(
            app.query.empty() ? "No games available for this account"
                              : "Nothing found for \"" + app.query + "\"",
            gfx::kWidth / 2, 480, gfx::FontSize::Body, gfx::kTextDim);
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
                "A  Play   Y  Search   ZL  Settings   X  Refresh   "
                "-  Sign out   +  Exit");
}

const char* kQualityLabels[3] = {"720p", "1080p", "1080p high bitrate"};
const char* kMappingLabels[2] = {"Positional (Switch A = Xbox B)",
                                 "Match labels (Switch A = Xbox A)"};

void draw_settings(App& app) {
    app.gfx.text("Settings", 60, 40, gfx::FontSize::Title, gfx::kText);

    struct Row {
        const char* title;
        std::string value;
    };
    Row rows[2] = {
        {"Stream quality", kQualityLabels[app.settings.quality]},
        {"Button layout", kMappingLabels[app.settings.mapping]},
    };
    for (int i = 0; i < 2; ++i) {
        SDL_Rect row = {120, 220 + i * 130, gfx::kWidth - 240, 100};
        app.gfx.fill(row, gfx::kCard);
        if (i == app.settings_cursor) app.gfx.frame(row, gfx::kCardFocus, 4);
        app.gfx.text(rows[i].title, row.x + 40, row.y + 30,
                     gfx::FontSize::Body, gfx::kText);
        app.gfx.text(rows[i].value,
                     row.x + row.w - 40 -
                         app.gfx.text_width(rows[i].value,
                                            gfx::FontSize::Body),
                     row.y + 30, gfx::FontSize::Body, gfx::kAccent);
    }
    app.gfx.text_centered(
        "Higher quality needs a stronger connection - 5 GHz Wi-Fi or "
        "docked LAN recommended for 1080p high bitrate",
        gfx::kWidth / 2, 620, gfx::FontSize::Small, gfx::kTextDim);
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
    draw_footer(app, "X  Retry   -  Sign out   +  Exit");
}

// ---- input ----------------------------------------------------------------

struct Input {
    bool a = false, b = false, x = false, y = false;
    bool up = false, down = false, left = false, right = false;
    bool plus = false, minus = false, zl = false;
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
                case SDLK_r: input.x = true; break;
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
    app.settings = load_settings();
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
                    join_worker(app);
                    running = false;
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
                if (input.y) {
                    app.query = keyboard_input(app.query);
                    apply_filter(app);
                }
                if (input.x) {
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
                if (input.zl) app.scene = Scene::Settings;
                if (input.a && !app.visible.empty()) {
                    app.launch_game = app.games[app.visible[app.cursor]];
#ifdef GNX_NATIVE_STREAM
                    app.engine->start(
                        app.launch_game.title_id,
                        static_cast<QualityTier>(app.settings.quality));
                    app.stream_hint_until = SDL_GetTicks() + 8000;
                    app.scene = Scene::Stream;
#endif
                }
                if (input.plus) running = false;
                break;
            }

            case Scene::Settings: {
                if (input.up) app.settings_cursor = 0;
                if (input.down) app.settings_cursor = 1;
                int direction = (input.right ? 1 : 0) - (input.left ? 1 : 0);
                if (direction != 0) {
                    if (app.settings_cursor == 0)
                        app.settings.quality =
                            (app.settings.quality + direction + 3) % 3;
                    else
                        app.settings.mapping =
                            (app.settings.mapping + direction + 2) % 2;
                    save_settings(app.settings);
                }
                if (input.b || input.zl) app.scene = Scene::Library;
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
                            static_cast<QualityTier>(app.settings.quality));
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
        }
        if (app.deko_active) {
            app.engine->pump_video();  // decodes + presents via deko3d
            // Pace input at ~125 Hz. Without SDL's vsync the loop can spin far
            // faster than the video rate; sending a gamepad packet every spin
            // floods the SCTP input channel ("sctp sendv error 11").
            Uint32 now = SDL_GetTicks();
            if (now - app.last_input_ms >= 8) {
                app.engine->send_gamepad(
                    read_gamepad(joystick, app.settings.mapping));
                app.last_input_ms = now;
            }
            SDL_Delay(1);  // yield between video frames instead of busy-spinning
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
