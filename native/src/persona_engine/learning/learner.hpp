#pragma once
// Learning loop (src/persona-engine/learning/learner.ts, PR-F / MISSING #3-1)。
// hypothesis が integrated / rejected されたタイミングで、提案した persona の learned_notes に結果を蓄積する。
// 提案者 persona は rule_log の detail から逆引きする ("persona <id> proposed hypothesis <hid>" パターン)。
#include <cstdint>
#include <optional>
#include <string>

#include "persona_engine/db/personas_repo.hpp"
#include "persona_engine/db/rules_repo.hpp"

namespace di::persona_engine::learning {

/** "integrated" | "rejected" */
using HypothesisOutcome = std::string;

struct RecordOutcomeArgs {
    db::PersonasRepo* personas = nullptr;
    db::RulesRepo* rules = nullptr;
    std::string hypothesisId;
    HypothesisOutcome outcome;
    /** 結果 hypothesis の statement (任意、note に埋め込む) */
    std::optional<std::string> statement;
    /** test 用 — 検索範囲を増やす (既定 500) */
    std::optional<std::int64_t> recentLogsLimit;
};

struct RecordOutcomeResult {
    bool recorded = false;
    std::optional<std::string> personaId;
};

RecordOutcomeResult record_hypothesis_outcome(const RecordOutcomeArgs& args);

struct ProposeMatch {
    std::string personaId;
    std::string hypothesisId;
};
/**
 * /persona\s+(\S+)\s+proposed\s+hypothesis\s+(\S+)/i の最初の一致 (exec と同じ)。正規表現を使わず線形に走査する。
 * metrics と共用。テスト用に公開。
 */
std::optional<ProposeMatch> match_propose_pattern(const std::string& detail);

/** learned note の文言 (✅ integrated: hyp:xxxxxxxx — "..." )。テスト用に公開。 */
std::string format_note(const HypothesisOutcome& outcome, const std::string& hypothesis_id,
                        const std::optional<std::string>& statement);

} // namespace di::persona_engine::learning
