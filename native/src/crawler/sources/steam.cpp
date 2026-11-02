#include "crawler/sources/steam.hpp"

#include <set>
#include <stdexcept>

#include "base/strings.hpp"
#include "base/url.hpp"

namespace di::crawler::sources {

namespace {
constexpr const char* USER_AGENT = "LUDIARS-Discutere-Crawler/0.1 (+https://github.com/LUDIARS)";
constexpr const char* SOURCE = "steam";

std::optional<double> opt_num(const Json& j, const char* k) {
    auto it = j.find(k);
    if (it != j.end() && it->is_number()) return it->get<double>();
    return std::nullopt;
}
} // namespace

std::optional<SteamReviewRaw> steam_review_from_json(const Json& j) {
    if (!j.is_object() || !j.contains("recommendationid") || !j["recommendationid"].is_string()) return std::nullopt;
    SteamReviewRaw r;
    r.recommendationid = j["recommendationid"].get<std::string>();
    if (j.contains("author") && j["author"].is_object()) {
        SteamReviewAuthor a;
        a.steamid = json_opt_string(j["author"], "steamid");
        a.num_games_owned = opt_num(j["author"], "num_games_owned");
        a.playtime_forever = opt_num(j["author"], "playtime_forever");
        r.author = a;
    }
    r.review = json_string(j, "review");
    r.language = json_opt_string(j, "language");
    r.timestamp_created = json_number(j, "timestamp_created");
    r.voted_up = json_bool(j, "voted_up");
    r.votes_up = opt_num(j, "votes_up");
    return r;
}

ExternalUtterance map_steam_review(long long app_id, const std::string& game_slug, const SteamReviewRaw& raw) {
    const std::string steam_id = raw.author && raw.author->steamid ? *raw.author->steamid : std::string("unknown");
    ExternalUtterance u;
    u.source = SOURCE;
    u.nativeId = raw.recommendationid;
    u.gameSlug = game_slug;
    u.threadKey = std::to_string(app_id);
    u.content = raw.review;
    u.lang = raw.language;
    u.postedAt = raw.timestamp_created * 1000;
    u.authorId = steam_id;  // SteamID64 = 公開・安定な persona アンカー (spec §6)
    // Steam レビューはスレッド構造を持たない (parentNativeId なし)
    u.signal = ExternalSignal{raw.voted_up, raw.votes_up};
    u.sourceUrl = "https://steamcommunity.com/profiles/" + steam_id + "/recommended/" + std::to_string(app_id) + "/";
    return u;
}

std::vector<ExternalUtterance> fetch_steam_reviews(const SteamFetchOptions& opts) {
    const std::vector<std::string> languages = opts.languages.empty() ? std::vector<std::string>{"all"} : opts.languages;
    const std::string lang_param = str::join(languages, ",");
    const std::string filter = opts.filter.value_or("recent");
    const long long delay = opts.politeDelayMs.value_or(1200);
    const std::size_t max_reviews = opts.maxReviews.value_or(0);
    const std::size_t max_bytes = 8u << 20;

    std::vector<ExternalUtterance> out;
    std::set<std::string> seen_cursors;
    std::string cursor = "*";
    for (std::size_t page = 0; page < opts.maxPages; ++page) {
        http::ClientRequest req;
        req.url = opts.base + "/appreviews/" + std::to_string(opts.appId) + "?json=1" + "&num_per_page=100&filter=" +
                  url::encode_component(filter) + "&language=" + url::encode_component(lang_param) +
                  "&review_type=all&purchase_type=all&cursor=" + url::encode_component(cursor);
        req.headers.set("User-Agent", USER_AGENT);
        req.max_response_bytes = max_bytes;
        auto res = do_fetch(opts.fetchImpl, req);
        if (!res.transport_ok) throw std::runtime_error("steam appreviews: " + res.error);
        if (!res.ok()) throw std::runtime_error("steam appreviews HTTP " + std::to_string(res.status));
        JsonLimits limits;
        limits.max_bytes = max_bytes;
        std::string error;
        auto data = parse_json(res.body, limits, &error);
        if (!data || !data->is_object()) throw std::runtime_error("steam appreviews: invalid JSON");
        const Json success = data->contains("success") ? (*data)["success"] : Json();
        if (!(success.is_number() && success.get<double>() == 1))
            throw std::runtime_error("steam appreviews success=" + (success.is_null() ? std::string("undefined") : json_stringify(success)));

        const Json& reviews = json_array(*data, "reviews");
        if (reviews.empty()) break;
        for (const auto& r : reviews) {
            auto raw = steam_review_from_json(r);
            if (!raw) continue;  // recommendationid が無いものは dedup key が作れないので捨てる
            out.push_back(map_steam_review(opts.appId, opts.gameSlug, *raw));
            if (max_reviews && out.size() >= max_reviews) break;
        }
        if (opts.onPage) opts.onPage(out.size());
        if (max_reviews && out.size() >= max_reviews) break;

        auto next = json_opt_string(*data, "cursor");
        if (!next || next->empty() || seen_cursors.count(*next)) break;  // 末尾 (cursor が進まなくなる) で停止
        seen_cursors.insert(*next);
        cursor = *next;
        polite_sleep(delay);
    }
    return out;
}

} // namespace di::crawler::sources
