#include "integrations/glab_review_trends.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

#include "base/strings.hpp"
#include "base/time.hpp"
#include "base/url.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"
#include "core/noise/exclusions.hpp"

namespace di::integrations {

namespace {

constexpr std::int64_t kMaxSafeInteger = 9007199254740991LL;

bool is_digit(char c) { return c >= '0' && c <= '9'; }

/** /\/(?:recommended|app)\/(\d+)(?:\/|$)/ の最初の一致の数字列 (線形の走査)。 */
std::optional<std::string_view> match_app_segment(std::string_view path) {
    static constexpr std::string_view kWords[] = {"recommended", "app"};
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] != '/') continue;
        for (const auto word : kWords) {
            const std::size_t w = i + 1;
            if (path.substr(w, word.size()) != word) continue;
            const std::size_t slash = w + word.size();
            if (slash >= path.size() || path[slash] != '/') continue;
            std::size_t d = slash + 1;
            while (d < path.size() && is_digit(path[d])) ++d;
            if (d == slash + 1) continue;
            if (d == path.size() || path[d] == '/') return path.substr(slash + 1, d - slash - 1);
        }
    }
    return std::nullopt;
}

} // namespace

Json to_json(const StoredReviewSignal& s) {
    return Json{{"gameSlug", s.gameSlug ? Json(*s.gameSlug) : Json(nullptr)},
                {"appId", s.appId ? Json(*s.appId) : Json(nullptr)},
                {"postedAt", s.postedAt},
                {"reaction", s.reaction ? Json(*s.reaction) : Json(nullptr)}};
}

Json to_json(const GameReviewTrend& t) {
    return Json{{"gameSlug", t.gameSlug},
                {"appId", t.appId ? Json(*t.appId) : Json(nullptr)},
                {"recentReviewCount", t.recentReviewCount},
                {"positivePercent", t.positivePercent ? Json(*t.positivePercent) : Json(nullptr)},
                {"latestReviewAt", t.latestReviewAt}};
}

std::vector<StoredReviewSignal> read_stored_steam_review_signals(sql::Database& db, const ListSteamAttributions& list) {
    const auto excluded = core::noise::list_excluded_ids(db);
    std::vector<StoredReviewSignal> records;
    if (!list) return records;
    for (const auto& source : list()) {
        if (excluded.count(source.utteranceId)) continue;
        const auto utterance = db.get("SELECT posted_at FROM utterances WHERE id = ?", {source.utteranceId});
        if (!utterance) continue;
        // importer が Steam の voted_up を保存する決定的 ID の reaction だけを採用する。
        // 後から同じ発話へ付いた positive/negative reaction でレビュー評価を上書きしない。
        const auto reaction = db.get("SELECT reaction_type FROM reactions WHERE id = ? AND utterance_id = ?",
                                     {source.utteranceId + ":vote", source.utteranceId});
        StoredReviewSignal s;
        s.gameSlug = source.gameSlug;
        s.appId = steam_app_id_from_source_url(source.sourceUrl);
        s.postedAt = (*utterance)["posted_at"].is_number() ? (*utterance)["posted_at"].as_double() : std::nan("");
        if (reaction) {
            const auto type = reaction->str("reaction_type");
            if (type == "positive" || type == "negative") s.reaction = type;
        }
        records.push_back(std::move(s));
    }
    return records;
}

std::vector<StoredReviewSignal> read_stored_steam_review_signals(sql::Database& db, crawler::sources::AttributionStore& attribution) {
    return read_stored_steam_review_signals(db, [&] { return attribution.list_by_source("steam", std::nullopt, kMaxSourceRecords); });
}

std::vector<GameReviewTrend> analyze_stored_review_trends(const std::vector<StoredReviewSignal>& records, std::optional<std::int64_t> now_ms,
                                                          int limit) {
    const double now = static_cast<double>(now_ms.value_or(time::now_ms()));
    const double cutoff = now - static_cast<double>(kReviewWindowMs);
    struct Group {
        double count = 0;
        double rated = 0;
        double positive = 0;
        double latest = 0;
        std::set<std::int64_t> appIds;
    };
    std::vector<std::pair<std::string, Group>> groups;  // Map の挿入順
    std::unordered_map<std::string, std::size_t> index;
    for (const auto& record : records) {
        const std::string slug = record.gameSlug ? str::trim(*record.gameSlug) : std::string();
        if (slug.empty() || !std::isfinite(record.postedAt) || record.postedAt < cutoff || record.postedAt > now) continue;
        auto it = index.find(slug);
        if (it == index.end()) {
            it = index.emplace(slug, groups.size()).first;
            groups.emplace_back(slug, Group{});
        }
        Group& g = groups[it->second].second;
        g.count += 1;
        g.latest = std::max(g.latest, record.postedAt);
        if (record.appId) g.appIds.insert(*record.appId);
        if (record.reaction) {
            g.rated += 1;
            if (*record.reaction == "positive") g.positive += 1;
        }
    }
    struct Entry {
        GameReviewTrend trend;
        double latest = 0;
    };
    std::vector<Entry> entries;
    for (const auto& [slug, g] : groups) {
        Entry e;
        e.trend.gameSlug = slug;
        if (g.appIds.size() == 1) e.trend.appId = *g.appIds.begin();
        e.trend.recentReviewCount = g.count;
        if (g.rated > 0) e.trend.positivePercent = std::floor((g.positive / g.rated) * 100 + 0.5);  // Math.round
        e.latest = std::floor(g.latest);
        e.trend.latestReviewAt = time::iso_utc(static_cast<std::int64_t>(g.latest));
        entries.push_back(std::move(e));
    }
    // localeCompare は ASCII の slug ではバイト順で近似する (port-notes に記載)。
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& l, const Entry& r) {
        if (l.trend.recentReviewCount != r.trend.recentReviewCount) return l.trend.recentReviewCount > r.trend.recentReviewCount;
        if (l.latest != r.latest) return l.latest > r.latest;
        return l.trend.gameSlug < r.trend.gameSlug;
    });
    std::vector<GameReviewTrend> out;
    const std::size_t n = static_cast<std::size_t>(std::max(0, limit));
    for (std::size_t i = 0; i < entries.size() && i < n; ++i) out.push_back(entries[i].trend);
    return out;
}

std::vector<GameReviewTrend> load_stored_steam_review_trends(std::optional<std::int64_t> now_ms, int limit) {
    const auto& cfg = config::get_config();
    core::CoreOptions co;
    co.sqlitePath = core::resolve_active_kg_path(cfg);
    auto core = core::create_core(co);
    struct CoreCloser {
        core::Core& c;
        ~CoreCloser() {
            try {
                c.close();
            } catch (...) {
            }
        }
    } core_closer{*core};
    auto attribution = crawler::sources::open_attribution_store();
    struct AttrCloser {
        crawler::sources::AttributionStore& a;
        ~AttrCloser() {
            try {
                a.close();
            } catch (...) {
            }
        }
    } attr_closer{*attribution};
    return analyze_stored_review_trends(read_stored_steam_review_signals(core->raw(), *attribution), now_ms, limit);
}

std::optional<std::int64_t> steam_app_id_from_source_url(std::string_view value) {
    const auto u = url::parse(value);
    if (!u || u->scheme != "https") return std::nullopt;
    if (u->host != "steamcommunity.com" && u->host != "store.steampowered.com") return std::nullopt;
    const auto digits = match_app_segment(u->path);
    if (!digits) return std::nullopt;
    // Number(match) → Number.isSafeInteger && > 0
    std::int64_t v = 0;
    for (const char c : *digits) {
        if (v > (kMaxSafeInteger - (c - '0')) / 10) return std::nullopt;
        v = v * 10 + (c - '0');
    }
    if (v <= 0) return std::nullopt;
    return v;
}

} // namespace di::integrations
