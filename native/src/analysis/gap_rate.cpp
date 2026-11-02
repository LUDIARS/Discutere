#include "analysis/gap_rate.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/sqlite.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "core/kg_registry.hpp"
#include "crawler/md_format.hpp"
#include "visualize/locale_compare.hpp"

namespace di::analysis {

namespace {

/** 読み出しの上限 (KG は大きいので行数・バイト数で課金する)。 */
constexpr sql::QueryLimits kSessionLimits{2'000'000, 512u << 20};
constexpr sql::QueryLimits kUtteranceLimits{5'000'000, 2048u << 20};
/** SQLite のバインド変数の上限 (32766) を超えないよう分割する境目。 */
constexpr std::size_t kMaxInParams = 30'000;

/** Math.round (0.5 は +∞ 側)。 */
double js_round(double x) { return std::floor(x + 0.5); }

double rate(double part, double whole) { return whole > 0 ? js_round((part / whole) * 1000) / 1000 : 0; }

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

double aspect_count(const AffectScore& score, const std::string& a) {
    for (const auto& [k, v] : score.aspects)
        if (k == a) return v;
    return 0;  // TS では undefined > 0 は false
}

GapClass classify(const AffectScore& score, const IntendedDef& intended) {
    if (score.valenceLabel == "negative") return "unintendedNegative";  // 運営=ポジ体験 と不一致
    bool on_intent = false;
    bool off_intent = false;
    for (const auto& a : score.aspectsHit) {
        if (intended.intendedAspects.count(a)) on_intent = true;
        else off_intent = true;
    }
    if (score.valenceLabel == "positive") return on_intent ? "match" : "offIntentPositive";
    // neutral: 想定外 aspect への言及があれば blind-spot、無シグナルは neutral
    return off_intent ? "blindSpot" : "neutral";
}

template <class KeyOf>
std::vector<SliceGap> slice_gap(const std::vector<ScoredUtterance>& items, KeyOf key_of) {
    std::vector<std::pair<std::string, std::vector<const ScoredUtterance*>>> groups;  // Map の挿入順
    for (const auto& u : items) {
        const std::string k = key_of(u);
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == k; });
        if (it == groups.end()) {
            groups.push_back({k, {}});
            it = groups.end() - 1;
        }
        it->second.push_back(&u);
    }
    std::vector<SliceGap> out;
    for (const auto& [key, us] : groups) {
        double signal = 0, gap = 0, neg = 0;
        for (const auto* u : us) {
            if (u->cls != "neutral") signal += 1;
            if (u->cls != "match" && u->cls != "neutral") gap += 1;
            if (u->cls == "unintendedNegative") neg += 1;
        }
        out.push_back({key, static_cast<double>(us.size()), rate(gap, signal), rate(neg, signal)});
    }
    std::stable_sort(out.begin(), out.end(), [](const SliceGap& a, const SliceGap& b) { return b.n < a.n; });
    return out;
}

/** Number#toFixed (同点は大きい方)。 */
std::string to_fixed(double x, int digits) {
    if (!std::isfinite(x) || std::fabs(x) >= 1e21) return js_number_to_string(x);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits, x);
    // printf は正確な 2 進値の同点を偶数側に丸める。JS は大きい方を選ぶので補正する。
    const double scale = std::pow(10.0, digits);
    const double scaled = x * scale;
    if (scaled * 2 == std::floor(scaled * 2) && scaled != std::floor(scaled) && (scaled - std::floor(scaled)) == 0.5) {
        std::snprintf(buf, sizeof buf, "%.*f", digits, (std::floor(scaled) + 1) / scale);
    }
    std::string s = buf;
    if (s == "-0" || s.rfind("-0.", 0) == 0) {
        // (-0.04).toFixed(1) は "-0.0"、(-0).toFixed(1) は "0.0"
        if (x == 0) s = s.substr(1);
    }
    return s;
}

std::string pct(double x) { return to_fixed(x * 100, 1) + "%"; }

std::string js_pad_end(const std::string& s, std::size_t width) {
    const std::size_t len = str::js_length(s);
    return len >= width ? s : s + std::string(width - len, ' ');
}
std::string js_pad_start(const std::string& s, std::size_t width) {
    const std::size_t len = str::js_length(s);
    return len >= width ? s : std::string(width - len, ' ') + s;
}
std::string num(double n) { return js_number_to_string(n); }

} // namespace

Json to_json(const MechanicGap& v) {
    return Json{{"name", v.name}, {"intendedValence", v.intendedValence}, {"intendedAspects", v.intendedAspects},
                {"attributed", v.attributed}, {"gapRate", v.gapRate}, {"negativeRate", v.negativeRate}};
}
Json to_json(const SliceGap& v) { return Json{{"key", v.key}, {"n", v.n}, {"gapRate", v.gapRate}, {"negativeRate", v.negativeRate}}; }
Json to_json(const AspectGap& v) {
    return Json{{"aspect", v.aspect}, {"intended", v.intended}, {"mentions", v.mentions}, {"negativeRate", v.negativeRate}};
}
Json to_json(const GapRateReport& v) {
    Json j = Json::object();
    j["game"] = v.game;
    j["slug"] = v.slug;
    j["total"] = v.total;
    j["breakdown"] = Json{{"match", v.breakdown.match},
                          {"offIntentPositive", v.breakdown.offIntentPositive},
                          {"unintendedNegative", v.breakdown.unintendedNegative},
                          {"blindSpot", v.breakdown.blindSpot},
                          {"neutral", v.breakdown.neutral}};
    j["matchRate"] = v.matchRate;
    j["gapRate"] = v.gapRate;
    j["neutralRate"] = v.neutralRate;
    j["negativeRate"] = v.negativeRate;
    Json a = Json::array();
    for (const auto& x : v.bySource) a.push_back(to_json(x));
    j["bySource"] = a;
    a = Json::array();
    for (const auto& x : v.byPeriod) a.push_back(to_json(x));
    j["byPeriod"] = a;
    a = Json::array();
    for (const auto& x : v.byAspect) a.push_back(to_json(x));
    j["byAspect"] = a;
    a = Json::array();
    for (const auto& x : v.byMechanic) a.push_back(to_json(x));
    j["byMechanic"] = a;
    return j;
}

std::string default_gap_kuzu_path() {
    // active KG (§12) を見る。未解決時は従来既定に倒す (完全後方互換)。
    if (auto p = core::resolve_active_kg_path(config::get_config())) return *p;
    if (auto e = env::get("DISCATIER_KUZU_PATH")) return *e;
    return fs::resolve("./data/discatier.kuzu");
}

IntendedDef load_intended(const std::string& slug, const std::optional<std::string>& games_dir) {
    const std::string dir = games_dir ? *games_dir : fs::resolve("./data/games");
    const auto safe = fs::safe_join(dir, slug + ".md");
    const std::string md_path = safe ? *safe : fs::join(dir, slug + ".md");
    if (!safe || !fs::is_file(md_path))
        throw std::runtime_error("GameKG md が見つかりません: " + md_path + " (運営の想定感情を定義してください)");
    const auto text = fs::read_file(md_path, kMaxGameKgBytes);
    if (!text) throw std::runtime_error("GameKG md を読めません (大きすぎる): " + md_path);
    const auto kg = crawler::parse_game_kg(*text, md_path);
    IntendedDef def;
    for (const auto& m : kg.mechanics) {
        std::vector<std::string> aspects = m.intended_aspects.value_or(std::vector<std::string>{});
        for (const auto& a : aspects) def.intendedAspects.insert(a);
        def.mechanics.push_back({m.name, m.intended_valence.value_or("positive"), aspects});
    }
    return def;
}

std::vector<ScoredUtterance> load_observed(const std::string& slug, const std::optional<std::string>& db_path) {
    sql::OpenOptions opts;
    opts.readonly = true;
    opts.fileMustExist = true;
    opts.create = false;
    auto db = sql::Database::open(db_path ? *db_path : default_gap_kuzu_path(), opts);
    struct Closer {
        sql::DatabasePtr db;
        ~Closer() { db->close(); }
    } closer{db};

    const std::string suffix = ":" + slug;
    std::vector<std::string> mine;
    std::map<std::string, std::string> by_session;
    for (const auto& s : db->all("SELECT id, title FROM sessions", {}, kSessionLimits)) {
        const std::string title = s.str("title");
        if (!str::ends_with(title, suffix)) continue;
        const std::string id = s.str("id");
        mine.push_back(id);
        const auto colon = title.find(':');
        std::string source = title.substr(0, colon);
        by_session[id] = source.empty() ? "unknown" : source;
    }
    std::vector<ScoredUtterance> out;
    if (mine.empty()) return out;

    for (std::size_t i = 0; i < mine.size(); i += kMaxInParams) {
        const std::size_t end = std::min(mine.size(), i + kMaxInParams);
        std::string placeholders;
        sql::Params params;
        for (std::size_t j = i; j < end; ++j) {
            placeholders += j == i ? "?" : ",?";
            params.add(mine[j]);
        }
        for (const auto& r : db->all("SELECT session_id, raw_content, posted_at FROM utterances WHERE session_id IN (" + placeholders + ")",
                                     params, kUtteranceLimits)) {
            ScoredUtterance u;
            auto it = by_session.find(r.str("session_id"));
            u.source = it != by_session.end() ? it->second : "unknown";
            const double posted = r.num("posted_at");
            u.period = posted != 0 && std::isfinite(posted) ? time::iso_utc(static_cast<std::int64_t>(posted)).substr(0, 7) : "unknown";
            u.score = score_text(r.str("raw_content"));
            out.push_back(std::move(u));
        }
    }
    return out;
}

GapRateReport analyze_gap_rate(const std::string& slug, const std::optional<std::string>& games_dir, const std::optional<std::string>& db_path) {
    const auto intended = load_intended(slug, games_dir);
    auto observed = load_observed(slug, db_path);
    for (auto& u : observed) u.cls = classify(u.score, intended);

    GapRateReport r;
    r.game = slug;
    r.slug = slug;
    r.total = static_cast<double>(observed.size());
    for (const auto& u : observed) {
        if (u.cls == "match") r.breakdown.match += 1;
        else if (u.cls == "offIntentPositive") r.breakdown.offIntentPositive += 1;
        else if (u.cls == "unintendedNegative") r.breakdown.unintendedNegative += 1;
        else if (u.cls == "blindSpot") r.breakdown.blindSpot += 1;
        else r.breakdown.neutral += 1;
    }
    // Gap率は「感情シグナルのある発話」を母数にする (中立=無シグナルは別軸=カバレッジ)。
    const double signal = r.total - r.breakdown.neutral;
    r.matchRate = rate(r.breakdown.match, signal);
    r.gapRate = signal > 0 ? js_round((1 - r.matchRate) * 1000) / 1000 : 0;
    r.negativeRate = rate(r.breakdown.unintendedNegative, signal);
    r.neutralRate = rate(r.breakdown.neutral, r.total);

    // 感情次元軸: aspect ごとの負感情率
    std::vector<std::string> aspect_keys;
    for (const auto& u : observed)
        for (const auto& a : u.score.aspectsHit)
            if (!contains(aspect_keys, a)) aspect_keys.push_back(a);
    for (const auto& a : aspect_keys) {
        double ment = 0, neg = 0;
        for (const auto& u : observed) {
            if (aspect_count(u.score, a) > 0) {
                ment += 1;
                if (u.score.valenceLabel == "negative") neg += 1;
            }
        }
        r.byAspect.push_back({a, intended.intendedAspects.count(a) > 0, ment, rate(neg, ment)});
    }
    std::stable_sort(r.byAspect.begin(), r.byAspect.end(), [](const AspectGap& x, const AspectGap& y) {
        if (y.negativeRate != x.negativeRate) return y.negativeRate < x.negativeRate;
        return y.mentions < x.mentions;
    });

    // mechanic 軸: aspect 重なりで帰属 → 想定 valence と不一致の割合
    for (const auto& m : intended.mechanics) {
        double attr = 0, mismatch = 0, neg = 0;
        for (const auto& u : observed) {
            bool hit = false;
            for (const auto& a : u.score.aspectsHit)
                if (contains(m.aspects, a)) hit = true;
            if (!hit) continue;
            attr += 1;
            if (u.score.valenceLabel != m.valence) mismatch += 1;
            if (u.score.valenceLabel == "negative") neg += 1;
        }
        r.byMechanic.push_back({m.name, m.valence, m.aspects, attr, rate(mismatch, attr), rate(neg, attr)});
    }
    std::stable_sort(r.byMechanic.begin(), r.byMechanic.end(), [](const MechanicGap& x, const MechanicGap& y) { return y.gapRate < x.gapRate; });

    r.bySource = slice_gap(observed, [](const ScoredUtterance& u) { return u.source; });
    r.byPeriod = slice_gap(observed, [](const ScoredUtterance& u) { return u.period; });
    std::stable_sort(r.byPeriod.begin(), r.byPeriod.end(),
                     [](const SliceGap& a, const SliceGap& b) { return visualize::locale_compare(a.key, b.key) < 0; });
    return r;
}

std::string format_gap_report(const GapRateReport& r) {
    std::vector<std::string> L;
    L.push_back("=== Gap率レポート: " + r.slug + " (総発話 " + num(r.total) + ") ===");
    L.push_back("感情シグナル率(カバレッジ) " + pct(1 - r.neutralRate) + "  ※残りは中立/無シグナル");
    L.push_back("[シグナル中] 一致率 " + pct(r.matchRate) + " / Gap率 " + pct(r.gapRate) + " / " + "意図外負(運営と違う) " + pct(r.negativeRate));
    L.push_back("内訳: 一致 " + num(r.breakdown.match) + " / 別正 " + num(r.breakdown.offIntentPositive) + " / " + "意図外負 " +
                num(r.breakdown.unintendedNegative) + " / 未語彙化 " + num(r.breakdown.blindSpot) + " / 中立 " + num(r.breakdown.neutral));
    L.push_back("\n[ソース別] gap / neg");
    for (const auto& s : r.bySource)
        L.push_back("  " + js_pad_end(s.key, 10) + " n=" + js_pad_start(num(s.n), 5) + "  gap " + pct(s.gapRate) + " / neg " + pct(s.negativeRate));
    L.push_back("\n[施策/月次] gap / neg");
    for (const auto& p : r.byPeriod)
        L.push_back("  " + js_pad_end(p.key, 10) + " n=" + js_pad_start(num(p.n), 5) + "  gap " + pct(p.gapRate) + " / neg " + pct(p.negativeRate));
    L.push_back("\n[感情次元] 言及あたり負率 = 運営と違う度 (★=運営想定aspect)");
    for (std::size_t i = 0; i < r.byAspect.size() && i < 10; ++i) {
        const auto& a = r.byAspect[i];
        L.push_back(std::string("  ") + (a.intended ? "★" : " ") + " " + js_pad_end(a.aspect, 13) + " 言及 " + js_pad_start(num(a.mentions), 5) +
                    "  neg " + pct(a.negativeRate));
    }
    L.push_back("\n[mechanic別] 想定 vs 観測の不一致率");
    for (std::size_t i = 0; i < r.byMechanic.size() && i < 12; ++i) {
        const auto& m = r.byMechanic[i];
        L.push_back("  " + js_pad_end(str::js_slice(m.name, 0, 22), 22) + " 想定" + js_pad_end(m.intendedValence, 8) + " 帰属" +
                    js_pad_start(num(m.attributed), 5) + "  gap " + pct(m.gapRate) + " / neg " + pct(m.negativeRate));
    }
    return str::join(L, "\n");
}

} // namespace di::analysis
