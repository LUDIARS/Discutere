#include "persona_bridge/impression_intake.hpp"

#include <cmath>

#include "base/strings.hpp"
#include "flow/user_voices/collect.hpp"

namespace di::persona_bridge {

namespace uv = flow::user_voices;

Json to_json(const ImpressionSkipped& s) { return Json{{"non_anonymous_field", s.non_anonymous_field}, {"invalid", s.invalid}}; }

Json to_json(const IngestResult& r) {
    return Json{{"received", r.received}, {"imported", r.imported}, {"embedded", r.embedded}, {"games", r.games}};
}

bool is_anonymous_impression(const Json& o) {
    if (!o.is_object()) return false;
    for (auto it = o.begin(); it != o.end(); ++it) {
        const std::string& k = it.key();
        if (k != "id" && k != "gameTitle" && k != "recommend" && k != "polarity" && k != "comment" && k != "createdAt") return false;
    }
    return true;
}

namespace {

/** /^[\w.:-]{1,128}$/ (\w は ASCII の英数字と _)。 */
bool is_valid_id(const std::string& id) {
    if (id.empty() || id.size() > kImpressionIdMax) return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool is_nonblank_within(const Json& v, std::size_t max_units) {
    if (!v.is_string()) return false;
    const std::string& s = v.get_ref<const std::string&>();
    return !str::trim(s).empty() && str::js_length(s) <= max_units;
}

} // namespace

bool is_valid_impression(const Json& o) {
    if (!o.is_object()) return false;
    if (!o.contains("id") || !o["id"].is_string() || !is_valid_id(o["id"].get<std::string>())) return false;
    if (!o.contains("gameTitle") || !is_nonblank_within(o["gameTitle"], kImpressionGameTitleMax)) return false;
    if (!o.contains("comment") || !is_nonblank_within(o["comment"], kImpressionCommentMax)) return false;
    if (!o.contains("createdAt") || !o["createdAt"].is_string() || !std::isfinite(crawler::sources::js_date_parse(o["createdAt"].get<std::string>())))
        return false;
    if (o.contains("recommend") && !o["recommend"].is_null() && !o["recommend"].is_boolean()) return false;
    if (o.contains("polarity") && !o["polarity"].is_null() && !o["polarity"].is_string()) return false;
    return true;
}

ParsedImpressionBatch parse_impression_batch(const Json& body) {
    if (!body.is_object() || !body.contains("impressions") || !body["impressions"].is_array())
        throw ImpressionBatchError("impressions must be an array");
    const Json& list = body["impressions"];
    if (list.size() > IMPRESSION_PUSH_MAX)
        throw ImpressionBatchError("impressions must be at most " + std::to_string(IMPRESSION_PUSH_MAX) + " items");
    ParsedImpressionBatch out;
    for (const auto& item : list) {
        if (!item.is_object()) {
            out.skipped.invalid += 1;
            continue;
        }
        if (!is_anonymous_impression(item)) {
            out.skipped.non_anonymous_field += 1;
            continue;
        }
        if (!is_valid_impression(item)) {
            out.skipped.invalid += 1;
            continue;
        }
        uv::VoluptasImpression i;
        i.id = item["id"].get<std::string>();
        i.gameTitle = str::trim(item["gameTitle"].get<std::string>());
        if (item.contains("recommend") && item["recommend"].is_boolean()) i.recommend = item["recommend"].get<bool>();
        if (item.contains("polarity") && item["polarity"].is_string()) i.polarity = item["polarity"].get<std::string>();
        i.comment = item["comment"].get<std::string>();
        i.createdAt = item["createdAt"].get<std::string>();
        out.impressions.push_back(std::move(i));
    }
    return out;
}

IngestResult ingest_impressions(const std::vector<uv::VoluptasImpression>& impressions, const IngestDeps& deps) {
    std::vector<crawler::sources::ExternalUtterance> items;
    items.reserve(impressions.size());
    for (const auto& i : impressions) items.push_back(uv::map_impression(uv::glab_game_slug(i.gameTitle), i));
    IngestResult r;
    for (const auto& item : items) {
        auto& slot = r.games[item.gameSlug];
        slot = (slot.is_number() ? slot.get<std::int64_t>() : 0) + 1;
    }
    if (items.empty()) return r;
    r.received = static_cast<int>(items.size());
    r.imported = deps.importItems ? deps.importItems(items) : 0;
    if (deps.embed) {
        try {
            std::vector<std::string> ids;
            ids.reserve(items.size());
            for (const auto& item : items) ids.push_back(uv::utterance_id_of(item));
            r.embedded = deps.embed(ids);
        } catch (const std::exception& e) {
            if (deps.warn) deps.warn(std::string("届いた感想のベクトル化に失敗 (キーワード検索で継続): ") + e.what());
        }
    }
    return r;
}

} // namespace di::persona_bridge
