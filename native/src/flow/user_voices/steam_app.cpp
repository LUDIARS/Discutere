#include "flow/user_voices/steam_app.hpp"

#include <cmath>
#include <set>
#include <stdexcept>

#include "base/strings.hpp"
#include "base/url.hpp"
#include "flow/user_voices/nfkc.hpp"

namespace di::flow::user_voices {
namespace {

constexpr std::string_view kStore = "https://store.steampowered.com";
constexpr std::string_view kUserAgent = "LUDIARS-Discutere-Crawler/0.1 (+https://github.com/LUDIARS)";

/** /[\s　・:：\-–—_!！?？'"「」『』()（）[\]【】]/ */
bool is_removed(std::uint32_t cp) {
    if (str::is_js_space(cp)) return true;
    switch (cp) {
    case 0x3000: case 0x30FB: case ':': case 0xFF1A: case '-': case 0x2013: case 0x2014: case '_': case '!':
    case 0xFF01: case '?': case 0xFF1F: case '\'': case '"': case 0x300C: case 0x300D: case 0x300E: case 0x300F:
    case '(': case ')': case 0xFF08: case 0xFF09: case '[': case ']': case 0x3010: case 0x3011:
        return true;
    default:
        return false;
    }
}

/** getJson: 非 2xx は nullopt、通信失敗・壊れた JSON は例外。 */
std::optional<Json> get_json(const std::string& url, const FetchFn& fetch_impl) {
    http::ClientRequest req;
    req.url = url;
    req.headers.set("User-Agent", std::string(kUserAgent));
    req.headers.set("Accept", "application/json");
    req.timeout = std::chrono::milliseconds(15'000);
    req.max_response_bytes = kSteamMaxResponseBytes;
    const auto res = fetch_impl ? fetch_impl(req) : http::fetch(req);
    if (!res.transport_ok) throw std::runtime_error("steam fetch failed: " + res.error);
    if (!res.ok()) return std::nullopt;
    std::string error;
    auto parsed = parse_json(res.body, JsonLimits{kSteamMaxResponseBytes, 200'000, 32}, &error);
    if (!parsed) throw std::runtime_error("steam response is not JSON: " + error);
    return parsed;
}

/** typeof id === "number" (整数の appId だけを受ける)。 */
std::optional<std::int64_t> app_id_of(const Json& item) {
    if (!item.is_object()) return std::nullopt;
    auto it = item.find("id");
    if (it == item.end() || !it->is_number()) return std::nullopt;
    if (it->is_number_float()) {
        const double d = it->get<double>();
        if (!std::isfinite(d) || d != std::floor(d) || std::fabs(d) > 9.0e15) return std::nullopt;
        return static_cast<std::int64_t>(d);
    }
    return it->get<std::int64_t>();
}

} // namespace

Json to_json(const SteamApp& app) { return Json{{"appId", app.appId}, {"name", app.name}, {"released", app.released}}; }

std::string normalize_game_name(std::string_view value) {
    const std::string lowered = js_to_lower(nfkc_approx(value));
    std::string out;
    out.reserve(lowered.size());
    for (std::uint32_t cp : str::codepoints(lowered))
        if (!is_removed(cp)) str::append_utf8(out, cp);
    return out;
}

bool is_same_game(std::string_view candidate, std::string_view query) {
    const std::string c = normalize_game_name(candidate);
    const std::string q = normalize_game_name(query);
    if (str::js_length(c) < 2 || str::js_length(q) < 2) return false;
    return str::contains(q, c) || str::contains(c, q);
}

std::optional<SteamApp> resolve_steam_app(const ResolveSteamAppArgs& args) {
    std::set<std::string> tried;
    for (const auto& term : args.terms) {
        const std::string t = str::trim(term);
        if (t.empty() || tried.count(t) || tried.size() >= static_cast<std::size_t>(std::max(0, args.maxQueries))) continue;
        tried.insert(t);
        auto search = get_json(std::string(kStore) + "/api/storesearch/?term=" + url::encode_component(t) + "&l=japanese&cc=JP",
                               args.fetchImpl);
        std::optional<std::int64_t> hit_id;
        std::string hit_name;
        if (search && search->is_object()) {
            for (const auto& item : json_array(*search, "items")) {
                auto id = app_id_of(item);
                auto name = json_opt_string(item, "name");
                if (id && name && is_same_game(*name, args.matchAgainst)) {
                    hit_id = id;
                    hit_name = *name;
                    break;
                }
            }
        }
        if (!hit_id) continue;
        const std::string id_text = std::to_string(*hit_id);
        auto details = get_json(std::string(kStore) + "/api/appdetails?appids=" + id_text + "&cc=jp&l=japanese", args.fetchImpl);
        if (!details || !details->is_object()) continue;
        auto entry_it = details->find(id_text);
        if (entry_it == details->end() || !entry_it->is_object()) continue;
        const Json& entry = *entry_it;
        if (!json_truthy(entry.value("success", Json(nullptr)))) continue;
        const Json& data = json_object(entry, "data");
        SteamApp app;
        app.appId = *hit_id;
        app.name = json_opt_string(data, "name").value_or(hit_name);
        const Json& release = json_object(data, "release_date");
        auto coming = release.find("coming_soon");
        app.released = coming != release.end() && coming->is_boolean() && !coming->get<bool>();
        return app;
    }
    return std::nullopt;
}

} // namespace di::flow::user_voices
