#include "catalog.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

#include "../../vendor/json.hpp"

using nlohmann::json;

namespace gnx {

namespace {

// Client fingerprint the GSSV backend expects; matches what xbox.com/play sends.
constexpr const char* kDeviceInfo =
    "X-MS-Device-Info: {\"appInfo\":{\"env\":{\"clientAppId\":\"www.xbox.com\","
    "\"clientAppType\":\"browser\",\"clientAppVersion\":\"26.1.97\","
    "\"clientSdkVersion\":\"10.3.7\",\"httpEnvironment\":\"prod\","
    "\"sdkInstallId\":\"\"}},\"dev\":{\"hw\":{\"make\":\"Microsoft\","
    "\"model\":\"unknown\",\"sdktype\":\"web\"},\"os\":{\"name\":\"android\","
    "\"ver\":\"22631.2715\",\"platform\":\"desktop\"},\"displayInfo\":"
    "{\"dimensions\":{\"widthInPixels\":1280,\"heightInPixels\":720},"
    "\"pixelDensity\":{\"dpiX\":1,\"dpiY\":1}},\"browser\":"
    "{\"browserName\":\"chrome\",\"browserVersion\":\"140.0.3485.54\"}}}";

}  // namespace

std::vector<HomeConsole> fetch_home_consoles(
    Http& http, const EndpointCredentials& home) {
    HttpResponse response =
        http.get(home.host + "/v6/servers/home",
                 {"Accept: application/json",
                  "Content-Type: application/json",
                  "X-Gssv-Client: XboxComBrowser", kDeviceInfo,
                  "Authorization: Bearer " + home.token});
    if (!response.ok())
        throw std::runtime_error("console list failed with HTTP " +
                                 std::to_string(response.status) + ": " +
                                 response.body.substr(0, 300));

    json parsed = json::parse(response.body, nullptr, false);
    std::vector<HomeConsole> consoles;
    if (parsed.is_discarded()) return consoles;
    for (const json& entry : parsed.value("results", json::array())) {
        HomeConsole console;
        console.server_id = entry.value("serverId", "");
        console.name = entry.value("serverName", "");
        console.console_type = entry.value("consoleType", "");
        console.power_state = entry.value("powerState", "");
        if (!console.server_id.empty()) consoles.push_back(std::move(console));
    }
    return consoles;
}

std::vector<Game> fetch_playable_titles(Http& http,
                                        const EndpointCredentials& cloud) {
    HttpResponse response =
        http.get(cloud.host + "/v2/titles",
                 {"Accept: application/json",
                  "Content-Type: application/json",
                  "X-Gssv-Client: XboxComBrowser", kDeviceInfo,
                  "Authorization: Bearer " + cloud.token});
    if (!response.ok())
        throw std::runtime_error("titles request failed with HTTP " +
                                 std::to_string(response.status) + ": " +
                                 response.body.substr(0, 300));

    json parsed = json::parse(response.body);
    std::vector<Game> games;
    for (const json& entry : parsed.value("results", json::array())) {
        std::string title_id = entry.value("titleId", "");
        if (title_id.empty()) continue;

        const json details = entry.value("details", json::object());
        bool playable = details.value("hasEntitlement", false);
        if (!playable) {
            const json programs = details.value("programs", json::array());
            const json subs = details.value("userSubscriptions", json::array());
            for (const json& program : programs)
                for (const json& sub : subs)
                    if (program == sub) { playable = true; break; }
        }
        if (!playable) continue;

        Game game;
        game.title_id = title_id;
        game.product_id = details.value("productId", "");
        games.push_back(std::move(game));
    }

    std::sort(games.begin(), games.end(),
              [](const Game& a, const Game& b) { return a.title_id < b.title_id; });
    games.erase(std::unique(games.begin(), games.end(),
                            [](const Game& a, const Game& b) {
                                return a.title_id == b.title_id;
                            }),
                games.end());
    return games;
}

void fetch_names(Http& http, std::vector<Game>& games,
                 const std::string& market, const std::string& language) {
    // displaycatalog accepts comma-separated bigIds; keep batches modest.
    constexpr size_t kBatch = 20;
    std::unordered_map<std::string, Game*> by_product;
    for (Game& game : games)
        if (!game.product_id.empty()) by_product[game.product_id] = &game;

    std::vector<std::string> ids;
    ids.reserve(by_product.size());
    for (const auto& [id, game] : by_product) ids.push_back(id);

    for (size_t start = 0; start < ids.size(); start += kBatch) {
        std::string big_ids;
        for (size_t i = start; i < std::min(start + kBatch, ids.size()); ++i) {
            if (!big_ids.empty()) big_ids += ",";
            big_ids += ids[i];
        }
        std::string url =
            "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=" +
            big_ids + "&market=" + market + "&languages=" + language +
            "&fieldsTemplate=Details";

        HttpResponse response = http.get(url);
        if (!response.ok()) continue;  // metadata is best-effort
        json parsed = json::parse(response.body, nullptr, false);
        if (parsed.is_discarded()) continue;

        for (const json& product : parsed.value("Products", json::array())) {
            std::string product_id = product.value("ProductId", "");
            auto found = by_product.find(product_id);
            if (found == by_product.end()) continue;

            const json localized =
                product.value("LocalizedProperties", json::array());
            if (localized.empty()) continue;
            const json& properties = localized.front();
            found->second->name = properties.value("ProductTitle", "");

            std::string poster, box_art;
            for (const json& image :
                 properties.value("Images", json::array())) {
                std::string purpose = image.value("ImagePurpose", "");
                if (purpose == "Poster") poster = image.value("Uri", "");
                else if (purpose == "BoxArt") box_art = image.value("Uri", "");
            }
            const std::string& uri = !poster.empty() ? poster : box_art;
            if (!uri.empty())
                found->second->box_art_url = "https:" + uri + "?h=300";
        }
    }
}

}  // namespace gnx
