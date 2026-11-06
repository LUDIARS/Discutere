#include "visualize/conclusion_cache.hpp"

#include <cmath>

#include "base/time.hpp"
#include "visualize/flow_conclusions.hpp"

namespace di::visualize {

namespace {

constexpr const char* kUpsertSql = R"SQL(
  INSERT INTO conclusion_cache
    (gap_id, kind, session_id, title, conclusion, utterance_count, aufhebung_count,
     discussion_volume, material_count, updated_at, cached_at)
  VALUES
    (@gapId, @kind, @sessionId, @title, @conclusion, @utteranceCount, @aufhebungCount,
     @discussionVolume, @materialCount, @updatedAt, @cachedAt)
  ON CONFLICT(gap_id) DO UPDATE SET
    kind = excluded.kind,
    session_id = excluded.session_id,
    title = excluded.title,
    conclusion = excluded.conclusion,
    utterance_count = excluded.utterance_count,
    aufhebung_count = excluded.aufhebung_count,
    discussion_volume = excluded.discussion_volume,
    -- materialCount は重い KG 計算なので、呼び出し側が算出値 (>=0) を渡した時だけ上書きし、
    -- 未算出 (-1, = 収束 write-through) では既存値を温存する。
    material_count = CASE
      WHEN excluded.material_count >= 0 THEN excluded.material_count
      ELSE conclusion_cache.material_count
    END,
    updated_at = excluded.updated_at,
    cached_at = excluded.cached_at
)SQL";

/** 一覧の行の上限 (DB の値も外部入力として課金する)。 */
constexpr sql::QueryLimits kCacheListLimits{1'000'000, 512u << 20};

ConclusionSummary row_to_summary(const sql::Row& r) {
    ConclusionSummary s;
    s.gapId = r.str("gap_id");
    s.kind = r.str("kind");
    s.sessionId = r.str("session_id");
    s.title = r.str("title");
    s.conclusion = r.opt_str("conclusion");
    s.utteranceCount = r.i64("utterance_count");
    s.aufhebungCount = r.i64("aufhebung_count");
    s.updatedAt = r.i64("updated_at");
    s.discussionVolume = r.i64("discussion_volume");
    s.materialCount = r.i64("material_count");
    return s;
}

} // namespace

void upsert_conclusion_cache(sql::Database& db, const ConclusionSummary& summary, const UpsertOptions& opts) {
    const std::int64_t now = opts.now ? *opts.now : time::now_ms();
    db.run(kUpsertSql, sql::Params::named({
                           {"gapId", summary.gapId},
                           {"kind", summary.kind},
                           {"sessionId", summary.sessionId},
                           {"title", summary.title},
                           {"conclusion", sql::Value(summary.conclusion)},
                           {"utteranceCount", summary.utteranceCount},
                           {"aufhebungCount", summary.aufhebungCount},
                           {"discussionVolume", opts.discussionVolume.value_or(summary.utteranceCount)},
                           {"materialCount", opts.materialCount.value_or(MATERIAL_COUNT_UNCOMPUTED)},
                           {"updatedAt", summary.updatedAt},
                           {"cachedAt", now},
                       }));
}

void write_through_flow_conclusion(sql::Database& db, const WriteThroughParams& params) {
    const std::int64_t now = params.updatedAt ? *params.updatedAt : time::now_ms();
    ConclusionSummary s;
    s.gapId = "flow:" + params.sessionId;
    s.kind = "flow";
    s.sessionId = params.sessionId;
    s.title = params.title;
    s.conclusion = params.conclusion;
    s.utteranceCount = params.utteranceCount;
    s.aufhebungCount = params.aufhebungCount;
    s.updatedAt = now;
    UpsertOptions o;
    o.discussionVolume = params.utteranceCount;
    o.now = now;
    upsert_conclusion_cache(db, s, o);
}

std::vector<ConclusionSummary> list_cached_conclusions(sql::Database& db, std::int64_t limit) {
    const auto rows = db.all(
        R"SQL(SELECT gap_id, kind, session_id, title, conclusion, utterance_count, aufhebung_count,
              discussion_volume, material_count, updated_at
         FROM conclusion_cache
        ORDER BY updated_at DESC
        LIMIT ?)SQL",
        {limit}, kCacheListLimits);
    std::vector<ConclusionSummary> out;
    out.reserve(rows.size());
    for (const auto& r : rows) out.push_back(row_to_summary(r));
    return out;
}

std::int64_t conclusion_cache_count(sql::Database& db) {
    const auto row = db.get("SELECT COUNT(*) AS n FROM conclusion_cache");
    return row ? row->i64("n") : 0;
}

void ensure_conclusion_cache_fresh(sql::Database& db) {
    const auto total = db.get("SELECT COUNT(*) AS n FROM flow_conclusion");
    const std::int64_t flow_total = total ? total->i64("n") : 0;
    if (conclusion_cache_count(db) >= flow_total) return;
    const auto flows = list_flow_conclusions(db, kMaxSafeInteger);
    db.transaction([&] {
        for (const auto& s : flows) {
            UpsertOptions o;
            o.discussionVolume = s.utteranceCount;
            upsert_conclusion_cache(db, s, o);
        }
    });
}

void set_material_count(sql::Database& db, const std::string& gap_id, std::int64_t count) {
    db.run("UPDATE conclusion_cache SET material_count = ? WHERE gap_id = ?", {count, gap_id});
}

std::int64_t recompute_material_counts(sql::Database& db, const std::function<double(const ConclusionSummary&)>& count_material,
                                       bool only_missing) {
    const auto rows = list_cached_conclusions(db, kMaxSafeInteger);
    std::vector<ConclusionSummary> targets;
    for (const auto& r : rows)
        if (!only_missing || r.materialCount.value_or(-1) < 0) targets.push_back(r);
    db.transaction([&] {
        for (const auto& r : targets) {
            double count = 0;
            try {
                count = count_material(r);
            } catch (...) {
                continue;  // 1 件の失敗で全体を止めない
            }
            if (std::isfinite(count) && count >= 0) db.run("UPDATE conclusion_cache SET material_count = ? WHERE gap_id = ?", {count, r.gapId});
        }
    });
    return static_cast<std::int64_t>(targets.size());
}

} // namespace di::visualize
