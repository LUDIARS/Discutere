#pragma once
// src/flow/debatability.ts の移植: 議論適性ゲート (09-paper-gate-debatability) — 情報ゲート後段の「質」チェック。
//   1. 争点の存在 (decompose_issues、LLM) 2. 両論武装可能性 (LLM 小モデル) 3. 証拠バランス (機械計算のみ)
// LLM 失敗はゲートを止めず degraded=true で「適性あり扱い」に倒す。依存はすべて注入できる。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "flow/dialectic/issues.hpp"
#include "flow/effect_predict.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

/** "both" | "pro-only" | "con-only" | "neither" */
using IssueArmable = std::string;

struct IssueArmability {
    std::string issue;
    IssueArmable armable;
};

struct DominantSource {
    std::string name;
    double share = 0;
};

struct EvidenceBalance {
    double voiceCount = 0;
    double positive = 0;
    double negative = 0;
    double neutral = 0;
    double polaritySkew = 0;
    std::optional<DominantSource> dominantSource;
};
Json to_json(const EvidenceBalance& e);

struct FlowRecommendation {
    /** "sparring" | "learning" */
    std::string flow;
    std::string reason;
};

struct DebatabilityResult {
    std::vector<std::string> issues;
    std::vector<IssueArmability> armability;
    int armableBothCount = 0;
    int minArmableIssues = 0;
    EvidenceBalance evidence;
    bool debatable = true;
    bool degraded = false;
    std::optional<FlowRecommendation> recommendation;
    std::string message;
};
Json to_json(const DebatabilityResult& r);

struct DebatabilityVoice {
    std::string content;
    std::optional<std::string> source;
};

using ToVectorFn = std::function<std::vector<double>(const std::string&)>;

/** 証拠バランスを機械計算する (valence 次元 dims[0] = (valence+1)/2 から極性を復元)。 */
EvidenceBalance compute_evidence_balance(const std::vector<DebatabilityVoice>& voices, const ToVectorFn& to_vector = {});

struct JudgeArmabilityArgs {
    std::string theme;
    std::vector<std::string> issues;
    std::vector<DebatabilityVoice> voices;
    std::optional<std::string> model;
    WarnFn warn;
};
/** issue ごとの両論武装可能性を 1 コールで判定する。LLM 失敗 / 解析不能は nullopt。 */
std::optional<std::vector<IssueArmability>> judge_issue_armability(persona_engine::LLMClient& llm,
                                                                    const JudgeArmabilityArgs& args);

using DecomposeFn = std::function<std::vector<std::string>(persona_engine::LLMClient&, const dialectic::DecomposeIssuesArgs&)>;

struct AssessDebatabilityArgs {
    std::string theme;
    std::optional<std::string> paperMd;
    std::vector<DebatabilityVoice> voices;
    int minArmableIssues = 2;
    std::optional<std::string> model;
    /** 論点分解の差し替え (未指定は dialectic::decompose_issues)。 */
    DecomposeFn decompose;
    /** 極性ベクトルの差し替え (未指定は text_to_vector)。 */
    ToVectorFn toVector;
    WarnFn warn;
};
/** 議論適性を評価する。llm は呼び出し側で with_cost_log 済みを渡す。 */
DebatabilityResult assess_debatability(persona_engine::LLMClient& llm, const AssessDebatabilityArgs& args);

/** `(両論: ○)` 等の注記。 */
std::string armable_annotation(const IssueArmable& armable);
/** `# 論点` 節に載せる注記付き論点行。 */
std::vector<std::string> annotated_issues(const DebatabilityResult& result);

} // namespace di::flow
