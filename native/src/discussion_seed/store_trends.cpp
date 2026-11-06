#include "discussion_seed/store_trends.hpp"

#include <algorithm>

#include "base/strings.hpp"
#include "infra/http_client.hpp"

namespace di::discussion_seed {

namespace {

/** 取得失敗時の静的フォールバック (代表的な話題ジャンル/作品を種にする)。 */
const std::vector<std::string>& fallback_titles() {
    static const std::vector<std::string> titles = {
        "オープンワールドサバイバルクラフト",
        "協力型ローグライト",
        "オートチェス / オートバトラー",
        "ライフシム",
        "抽出シューター (extraction shooter)",
    };
    return titles;
}

SeedTopic to_topic(const std::string& title) {
    return {"【話題】" + title + ": 何が支持されている?",
            "ストアで注目されている「" + title + "」。 プレイヤーに支持されている要素や、 議論されている点を" +
                " 多角的に掘り下げ、 面白さ・課題・流行の理由を整理する。"};
}

/** JS の Array#slice(0, limit)。 */
std::vector<std::string> slice_head(const std::vector<std::string>& v, std::int64_t limit) {
    const auto n = static_cast<std::int64_t>(v.size());
    std::int64_t end = limit < 0 ? std::max<std::int64_t>(0, n + limit) : std::min(limit, n);
    return {v.begin(), v.begin() + end};
}

}  // namespace

std::vector<std::string> fetch_steam_top_seller_names(std::int64_t limit, const StoreTrendSource& source) {
    http::ClientRequest req;
    req.url = source.url;
    req.timeout = source.timeout;
    req.connect_timeout = std::min(source.timeout, std::chrono::milliseconds(8000));
    req.max_response_bytes = kSteamFeaturedMaxBytes;
    req.allow_private_network = source.allow_private_network;
    const auto res = http::fetch(req);
    if (!res.ok()) return {};
    auto json = parse_json(res.body, JsonLimits{kSteamFeaturedMaxBytes, 200'000, 32});
    if (!json || !json->is_object()) return {};
    const Json& top = json_object(*json, "top_sellers");
    const Json& items = json_array(top, "items");
    std::vector<std::string> names;
    for (const auto& raw : items) {
        const std::string name = raw.is_object() && raw.contains("name") && raw["name"].is_string()
                                     ? str::trim(raw["name"].get<std::string>())
                                     : "";
        if (!name.empty()) names.push_back(name);
        if (static_cast<std::int64_t>(names.size()) >= limit) break;
    }
    return names;
}

std::vector<SeedTopic> fetch_store_trend_topics(std::int64_t limit, const StoreTrendSource& source) {
    const auto names = fetch_steam_top_seller_names(limit, source);
    const auto list = names.empty() ? slice_head(fallback_titles(), limit) : names;
    std::vector<SeedTopic> out;
    for (const auto& n : list) out.push_back(to_topic(n));
    return out;
}

} // namespace di::discussion_seed
