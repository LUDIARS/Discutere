#pragma once
// AI の応答 JSON を action に dispatch する (src/persona-engine/engine/handler.ts)。
//
// Phase 0 で受け付ける action:
//   - skip (何もしない)
//   - propose_hypothesis (DiscussionContextProvider::propose_hypothesis)
//   - post_utterance (DiscussionContextProvider::post_utterance)
//   - add_rule / remove_rule (AI による rule の自己補正。禁則・seed 保護あり)
//
// 不正 JSON / 未知 action / 必須フィールド欠落 はすべて error として RulesRepo::log に残し、
// 例外を投げない (engine 全体は走り続ける)。
#include <optional>
#include <string>

#include "base/json.hpp"
#include "persona_engine/context_provider.hpp"
#include "persona_engine/db/rules_repo.hpp"
#include "persona_engine/types.hpp"

namespace di::persona_engine::engine {

/** 発火のきっかけになった event ({kind, payload?})。 */
struct TriggerEvent {
    std::string kind;
    std::optional<Json> payload;
};

struct HandleActionArgs {
    std::string ruleId;
    std::string personaId;
    std::string workspaceId;
    std::optional<std::string> sessionId;
    std::optional<TriggerEvent> triggerEvent;
    std::string rawText;
    DiscussionContextProvider* contextProvider = nullptr;
    db::RulesRepo* rules = nullptr;
    Logger* logger = nullptr;
};

struct HandleActionResult {
    /** "skip" | "propose_hypothesis" | "post_utterance" | "add_rule" | "remove_rule" | "error" */
    std::string kind;
    std::string detail;
    std::optional<std::string> createdId;
};

struct ForbiddenRuleCheck {
    bool ok = true;
    std::string reason;
};

/**
 * AI が add_rule で持ち込もうとする rule の禁則 (= 議論を破壊する rule の事前 reject)。
 * TS の FORBIDDEN_RULE_PATTERNS (正規表現 /i) と同じ判定を線形の走査で行う。
 */
ForbiddenRuleCheck check_forbidden_rule(const std::string& id, const std::optional<std::string>& description,
                                        const std::string& instructions);

HandleActionResult handle_action(const HandleActionArgs& args);

/** LLM 応答の JSON を読む上限 (外部入力)。 */
JsonLimits action_json_limits();
/** 波括弧の釣り合いで候補を試す回数の上限 (病的な入力で O(n^2) にしない)。 */
inline constexpr int kMaxExtractAttempts = 256;

/** LLM 応答から最初の JSON object を抽出 (周辺の説明文を許容)。テスト用に公開 (TS の __test.extractJson)。 */
std::optional<Json> extract_json(const std::string& text);

} // namespace di::persona_engine::engine
