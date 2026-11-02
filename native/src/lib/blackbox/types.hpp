#pragma once
// 成長型ブラックボックス — ドメイン非依存の中核型 (lib/lapilli/packages/blackbox/src/types.ts)。
// 永続化 (RuleStore / DecisionLedger) と LLM (LlmFallback) は利用側が注入する。
//
// C++ では特徴量・条件・判断結果を di::Json で持つ (TS の FeatureMap / Condition / unknown)。
// - FeatureMap: JSON object (値は number | string | boolean)。
// - Condition : 直列化可能な述語 AST の JSON ({op:'cmp'|'in'|'and'|'or'|'not', ...})。
//   DB・ファイルと同じ形のまま持つので、保存 → 読み戻しでキー順が変わらない。
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::lib::blackbox {

using FeatureMap = Json;
using Condition = Json;

/** 'candidate' | 'trial' | 'auto' | 'retired' */
using RuleState = std::string;

/** ルール = 「条件が成立したらこの output を返す」 を表すデータ。 */
struct Rule {
    std::string id;
    std::string domain;
    std::string description;
    Condition when;
    /** 条件成立時の判断結果 (JSON 直列化可能な任意値)。 */
    Json output;
    double confidence = 0.7;  // 0..1
    RuleState state = "candidate";
    std::string source = "manual";  // 'llm' | 'manual' | 'seed'
    std::int64_t approvals = 0;
    std::int64_t rejections = 0;
    std::int64_t shadowAgreements = 0;
    std::int64_t shadowConflicts = 0;
    std::int64_t proposals = 1;
    /** when + output の正規化ハッシュ。重複提案のマージと撤回後の再提案ブロックに使う。 */
    std::string fingerprint;
    double priority = 0;  // 同 domain 内の適用順 (大きいほど先)
    std::string createdAt;
    std::string updatedAt;
};

/** ルールを作るための入力 (id / fingerprint / 監査列はストアが補完)。 */
struct RuleDraft {
    std::string domain;
    std::string description;
    Condition when;
    Json output;  // null = 未指定 (TS の undefined ?? null)
    std::optional<double> confidence;
    std::optional<RuleState> state;
    std::optional<std::string> source;
    std::optional<double> priority;
};

/** LLM が返すルール候補 (Omit<RuleDraft, 'domain' | 'state' | 'source'>)。 */
struct RuleProposal {
    std::string description;
    Condition when;
    Json output;
    std::optional<double> confidence;
    std::optional<double> priority;
};

/** 判断の結果 + 来歴。 */
struct Decision {
    Json output;
    std::string source;  // 'rule' | 'llm'
    std::optional<std::string> ruleId;
    double confidence = 0;
    std::string rationale;
    /** 'auto' | 'pending_review' */
    std::string status;
};

/** 1 判断で candidate ルールを影評価した結果。 */
struct ShadowEval {
    std::string ruleId;
    bool agreed = false;
    bool operator==(const ShadowEval&) const = default;
};

/** ledger に残る 1 判断のレコード。 */
struct DecisionRecord {
    std::int64_t id = 0;
    std::string domain;
    Json input;
    FeatureMap features = Json::object();
    Json output;
    std::string source;
    std::optional<std::string> ruleId;
    double confidence = 0;
    std::string rationale;
    std::string status;
    std::optional<std::string> verdict;  // 'ok' | 'ng' | null
    std::vector<ShadowEval> shadow;
    std::string createdAt;
    std::optional<std::string> reviewedAt;
};

/** record() に渡す入力 (Omit<DecisionRecord, 'id' | 'verdict' | 'reviewedAt'>)。 */
struct DecisionInput {
    std::string domain;
    Json input;
    FeatureMap features = Json::object();
    Json output;
    std::string source;
    std::optional<std::string> ruleId;
    double confidence = 0;
    std::string rationale;
    std::string status;
    std::vector<ShadowEval> shadow;
    std::string createdAt;
};

struct RetiredRuleInfo {
    std::string description;
    std::string whenText;
};

/** LLM フォールバックに渡す文脈。撤回済みルールを知らせて同じ失敗の再提案を防ぐ。 */
struct LlmContext {
    std::string domain;
    std::vector<RetiredRuleInfo> retiredRules;
};

/** LLM フォールバックの戻り値。ルール化可能ならルール候補も返せる。 */
struct LlmJudgement {
    Json output;
    double confidence = 0;
    std::string rationale;
    std::optional<RuleProposal> proposedRule;
};

/** raw 入力から LLM 判断を得る関数 (同期。利用側で LLM 呼び出しを束ねて注入)。 */
using LlmFallback = std::function<LlmJudgement(const Json& input, const FeatureMap& features, const LlmContext& context)>;

struct RulePatch {
    std::optional<RuleState> state;
    std::optional<std::int64_t> approvals;
    std::optional<std::int64_t> rejections;
    std::optional<std::int64_t> shadowAgreements;
    std::optional<std::int64_t> shadowConflicts;
    std::optional<std::int64_t> proposals;
    std::optional<double> confidence;
    std::optional<double> priority;
    std::optional<std::string> description;
};

/** patch を rule に重ねる ({ ...cur, ...patch })。 */
inline void apply_patch(Rule& rule, const RulePatch& p) {
    if (p.state) rule.state = *p.state;
    if (p.approvals) rule.approvals = *p.approvals;
    if (p.rejections) rule.rejections = *p.rejections;
    if (p.shadowAgreements) rule.shadowAgreements = *p.shadowAgreements;
    if (p.shadowConflicts) rule.shadowConflicts = *p.shadowConflicts;
    if (p.proposals) rule.proposals = *p.proposals;
    if (p.confidence) rule.confidence = *p.confidence;
    if (p.priority) rule.priority = *p.priority;
    if (p.description) rule.description = *p.description;
}

/** ルールの永続化境界。 */
class RuleStore {
public:
    virtual ~RuleStore() = default;
    virtual std::vector<Rule> list_by_domain(const std::string& domain) = 0;
    virtual std::optional<Rule> find_by_fingerprint(const std::string& domain, const std::string& fingerprint) = 0;
    virtual Rule insert(const RuleDraft& draft) = 0;
    virtual std::optional<Rule> update(const std::string& id, const RulePatch& patch) = 0;
    virtual std::optional<Rule> get(const std::string& id) = 0;
};

/** 判断 ledger の永続化境界。 */
class DecisionLedger {
public:
    virtual ~DecisionLedger() = default;
    virtual std::int64_t record(const DecisionInput& rec) = 0;
    virtual std::optional<DecisionRecord> get(std::int64_t id) = 0;
    virtual void set_verdict(std::int64_t id, const std::string& verdict, const std::string& reviewed_at) = 0;
    /** レビュー待ちキュー: pending_review でまだ verdict が無い判断。 */
    virtual std::vector<DecisionRecord> list_pending(const std::optional<std::string>& domain = std::nullopt,
                                                     std::int64_t limit = 50) = 0;
    /** 直近の判断 (新しい順)。 */
    virtual std::vector<DecisionRecord> list_recent(const std::string& domain, std::int64_t limit) = 0;
};

// ─── JSON (API 応答・ファイルストアと同じ形。キー順は TS のオブジェクトリテラル順) ───
Json to_json(const Rule& rule);
Json to_json(const ShadowEval& e);
Json to_json(const DecisionRecord& rec);
Json to_json(const Decision& d);
/** 信頼できない JSON (ファイル) から読む。必須項目の型が違えば std::nullopt。 */
std::optional<Rule> rule_from_json(const Json& j);
std::optional<DecisionRecord> decision_record_from_json(const Json& j);
std::vector<ShadowEval> shadow_from_json(const Json& j);

} // namespace di::lib::blackbox
