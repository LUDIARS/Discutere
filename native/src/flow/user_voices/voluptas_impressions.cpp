#include "flow/user_voices/voluptas_impressions.hpp"

#include <algorithm>
#include <cmath>

#include "base/strings.hpp"
#include "base/time.hpp"
#include "base/url.hpp"

namespace di::flow::user_voices {

using crawler::sources::ExternalSignal;
using crawler::sources::ExternalUtterance;

crawler::sources::ExternalUtterance map_impression(const std::string& game_slug, const VoluptasImpression& impression) {
    const double posted_at = crawler::sources::js_date_parse(impression.createdAt);
    ExternalUtterance u;
    u.source = "glab";
    u.nativeId = impression.id;
    u.gameSlug = game_slug;
    u.threadKey = impression.gameTitle;
    u.content = impression.comment;
    u.postedAt = std::isfinite(posted_at) ? posted_at : static_cast<double>(time::now_ms());
    u.authorId = GLAB_ANONYMOUS_AUTHOR;
    if (impression.recommend) u.signal = ExternalSignal{impression.recommend, std::nullopt};
    // 出所の識別子 (公開 URL は無い)。書き手の情報は含まない。
    u.sourceUrl = "glab://impressions/" + url::encode_component(impression.id);
    return u;
}

std::optional<VoluptasImpression> impression_from_json(const Json& v) {
    if (!v.is_object()) return std::nullopt;
    for (const char* k : {"id", "gameTitle", "comment"})
        if (!v.contains(k) || !v[k].is_string()) return std::nullopt;
    if (str::trim(v["comment"].get<std::string>()).empty()) return std::nullopt;
    VoluptasImpression i;
    i.id = v["id"].get<std::string>();
    i.gameTitle = v["gameTitle"].get<std::string>();
    i.recommend = json_opt_bool(v, "recommend");
    i.polarity = json_opt_string(v, "polarity");
    i.comment = v["comment"].get<std::string>();
    // createdAt が文字列でなければ Date.parse(undefined) と同じく NaN → 現在時刻になる。
    i.createdAt = json_string(v, "createdAt");
    return i;
}

std::vector<crawler::sources::ExternalUtterance> fetch_voluptas_impressions(const FetchVoluptasArgs& args) {
    auto base = url::parse(args.baseUrl);
    if (!base || (base->scheme != "http" && base->scheme != "https")) throw std::invalid_argument("Invalid URL");
    const bool loopback = base->host == "localhost" || base->host == "127.0.0.1" || base->host == "::1";
    if (base->scheme != "https" && !loopback) throw std::runtime_error("Voluptas URL must use HTTPS (HTTP is allowed only on loopback)");
    const int limit = std::max(1, std::min(args.limit, 200));
    // new URL("/api/personas/impressions", baseUrl) は base の path/query を捨てて origin に付ける。
    http::ClientRequest req;
    req.url = base->origin() + "/api/personas/impressions?" + url::build_query({{"game", args.game}, {"limit", std::to_string(limit)}});
    req.max_redirects = 0;  // redirect: "manual"
    req.timeout = std::chrono::milliseconds(10'000);
    req.connect_timeout = std::chrono::milliseconds(10'000);
    req.max_response_bytes = 4u << 20;
    // 運用者が設定した Voluptas (loopback または LAN の https) へ繋ぐので private network を許す。
    req.allow_private_network = true;
    req.headers.set("Authorization", "Bearer " + args.bearer);
    req.headers.set("Accept", "application/json");
    auto res = args.fetchImpl ? args.fetchImpl(req) : http::fetch(req);
    if (!res.transport_ok) throw VoluptasUnavailable();
    if (res.status >= 500) throw VoluptasUnavailable();
    if (!res.ok()) throw std::runtime_error("Voluptas impressions request failed with status " + std::to_string(res.status));
    JsonLimits limits;
    limits.max_bytes = 4u << 20;
    std::string error;
    auto body = parse_json(res.body, limits, &error);
    if (!body) throw std::runtime_error("Voluptas impressions: invalid JSON");
    std::vector<ExternalUtterance> out;
    if (!body->is_object()) return out;
    const Json& impressions = json_array(json_object(*body, "data"), "impressions");
    for (const auto& item : impressions)
        if (auto i = impression_from_json(item)) out.push_back(map_impression(args.gameSlug, *i));
    return out;
}

} // namespace di::flow::user_voices
