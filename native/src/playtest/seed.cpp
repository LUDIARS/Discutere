// @implements SPEC-DI-AI-PLAYTEST-REVIEWS
#include "playtest/seed.hpp"

#include <algorithm>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/strings.hpp"
#include "playtest/voice_store.hpp"
#include "playtest/contracts/runtime.hpp" /* augur-inject:import:e04790ac */
#include "contracts/seed.hpp" /* augur-inject:contract-predicate:2aeac4bd */

namespace di::playtest {
namespace {

struct Cursor {
    std::int64_t seq = 0;
    std::int64_t seededAt = 0;
};

Cursor load_cursor(sql::Database& db, const std::string& workspace, const std::string& key) {
    const auto row = db.get("SELECT seq, seeded_at FROM playtest_seed_cursor WHERE workspace_id = ? AND voice_key = ?", {workspace, key});
    return row ? Cursor{row->i64("seq"), row->i64("seeded_at")} : Cursor{};
}

void save_cursor(sql::Database& db, const std::string& workspace, const std::string& key, std::int64_t seq, std::int64_t now) {
    db.run(R"SQL(INSERT INTO playtest_seed_cursor VALUES (?, ?, ?, ?)
                 ON CONFLICT(workspace_id, voice_key) DO UPDATE SET seq = excluded.seq, seeded_at = excluded.seeded_at)SQL",
           {workspace, key, seq, now});
}

std::string human_section(sql::Database& db, const std::string& title, std::int64_t low, std::int64_t high) {
    std::string section;
    const auto rows = db.all(R"SQL(SELECT comment FROM human_playtest_voice WHERE game_title = ? AND seq > ? AND seq <= ?
                                  ORDER BY seq DESC LIMIT 5)SQL", {title, low, high});
    if (rows.empty()) return section;
    section = "## 人の試遊感想\n\n";
    for (const auto& row : rows) {
        const auto text = str::js_slice(row.str("comment"), 0, 800);
        for (const auto& line : str::split(text, "\n")) section += "> " + line + "\n";
        section += ">\n";
    }
    return section;
}

} // namespace

bool voice_threshold_met(std::int64_t count, std::int64_t minimum) { return ::augur::invoke_observed(::augur::ContractSpec{"C-6", "8071814e", "src/playtest/seed.cpp:46"}, di::playtest::checks::seed, [&]() -> bool { /* augur-inject:contract-wrap:8071814e */
    return minimum > 0 && count >= minimum;
}, count, minimum); /* augur-inject:contract-end:8071814e */ }

std::optional<PlaytestSeed> seed_from_playtest_voices(
    sql::Database& voices, const discussion_seed::SeedHeadlessDiscussionArgs& base, const VoiceSeedPolicy& policy, std::int64_t now) {
    if (!base.core || !base.personas) throw std::invalid_argument("playtest seed requires core and personas");
    auto& db = base.core->raw();
    db.exec(R"SQL(CREATE TABLE IF NOT EXISTS playtest_seed_cursor (
         workspace_id TEXT NOT NULL, voice_key TEXT NOT NULL, seq INTEGER NOT NULL, seeded_at INTEGER NOT NULL,
         PRIMARY KEY (workspace_id, voice_key)))SQL");
    return db.transaction([&]() -> std::optional<PlaytestSeed> {
        // Read one consistent snapshot. A new receipt arriving during seeding remains available for the next tick.
        const auto candidates = voices.all(R"SQL(WITH latest AS (
            SELECT MAX(seq) AS seq FROM ai_playtest_review GROUP BY game_id
          ), games AS (
            SELECT game_id, game_title, seq AS ai_max FROM ai_playtest_review JOIN latest USING(seq)
          ), titles AS (
            SELECT game_title, COUNT(*) AS matches FROM games GROUP BY game_title
          ), humans AS (
            SELECT game_title, MAX(seq) AS human_max FROM human_playtest_voice GROUP BY game_title
          )
          SELECT game_id, games.game_title AS title, ai_max,
                 CASE WHEN matches = 1 THEN COALESCE(human_max, 0) ELSE 0 END AS human_max
          FROM games JOIN titles USING(game_title) LEFT JOIN humans USING(game_title)
          UNION ALL
          SELECT NULL, game_title, 0, human_max FROM humans WHERE game_title NOT IN (SELECT game_title FROM titles)
          ORDER BY title, game_id)SQL");
        for (const auto& candidate : candidates) {
            const auto title = candidate.str("title");
            const auto game_id = candidate.opt_str("game_id");
            const auto ai_key = game_id ? "ai:" + *game_id : std::string{};
            const auto human_key = "human:" + title;
            const auto ai_max = candidate.i64("ai_max");
            const auto human_max = candidate.i64("human_max");
            const auto ai = game_id ? load_cursor(db, base.workspaceId, ai_key) : Cursor{};
            const auto human = human_max ? load_cursor(db, base.workspaceId, human_key) : Cursor{};
            const auto last = std::max(ai.seededAt, human.seededAt);
            if (last > 0 && now - last < policy.cooldownMs) continue;

            const auto gap_title = "試遊感想から「" + title + "」の改善を考える";
            if (db.get(R"SQL(SELECT 1 FROM design_gaps WHERE workspace_id = ? AND title = ?
                       AND COALESCE(status, 'open') NOT IN ('closed', 'converged', 'dismissed') LIMIT 1)SQL",
                       {base.workspaceId, gap_title})) continue;
            const auto human_count = voices.get("SELECT COUNT(*) AS n FROM human_playtest_voice WHERE game_title = ? AND seq > ? AND seq <= ?",
                                                {title, human.seq, human_max})->i64("n");
            const auto ai_count = game_id ? voices.get("SELECT COUNT(*) AS n FROM ai_playtest_review WHERE game_id = ? AND seq > ? AND seq <= ?",
                                                       {*game_id, ai.seq, ai_max})->i64("n") : 0;
            if (!voice_threshold_met(human_count + ai_count, policy.minNewVoices)) continue;

            auto args = base;
            args.topic.title = gap_title;
            args.topic.description = "試遊の観察を比較し、具体的な改善案とその理由をまとめてください。\n\n";
            args.topic.description += human_section(voices, title, human.seq, human_max);
            if (game_id) args.topic.description += "\n" + ai_review_section(voices.all(
                "SELECT payload FROM ai_playtest_review WHERE game_id = ? AND seq > ? AND seq <= ? ORDER BY seq DESC LIMIT 3",
                {*game_id, ai.seq, ai_max}));
            args.gameId = game_id.value_or("glab:" + crypto::sha256_hex(title));
            args.origin = "auto:playtest-voices";
            const auto seeded = discussion_seed::seed_headless_discussion(args);
            if (game_id) save_cursor(db, base.workspaceId, ai_key, ai_max, now);
            if (human_max) save_cursor(db, base.workspaceId, human_key, human_max, now);
            return PlaytestSeed{seeded.gapId, gap_title};
        }
        return std::nullopt;
    });
}

} // namespace di::playtest
