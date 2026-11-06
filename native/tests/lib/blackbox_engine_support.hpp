#pragma once
// blackbox_engine_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// @ludiars/blackbox engine.test.ts の移植 (学習ループ)。
#include "check.hpp"

#include "lib/blackbox/blackbox.hpp"

using namespace di;
using namespace di::lib::blackbox;

namespace blackbox_engine_support {
const Json COND_RAIN = Json{{"op", "cmp"}, {"feature", "peakAgreement"}, {"cmp", ">="}, {"value", 0.6}};
const Json RAINY = Json{{"peakAgreement", 0.8}};
const Json DRY = Json{{"peakAgreement", 0.1}};

struct Fixture {
    std::shared_ptr<MemoryRuleStore> rules = std::make_shared<MemoryRuleStore>();
    std::shared_ptr<MemoryDecisionLedger> ledger = std::make_shared<MemoryDecisionLedger>();
    BlackBoxEngine engine;
    explicit Fixture(const EngineOptions& opts = {}) : engine(rules, ledger, opts) {}
};

struct Overrides {
    std::optional<Json> output;
    bool dropProposal = false;
};

/** proposedRule 付きで {rain:true} を返す LLM フォールバック。呼び出し回数を数える。 */
LlmFallback rain_llm(int& calls, Overrides o = {}) {
    return [&calls, o](const Json&, const Json&, const LlmContext&) {
        calls += 1;
        LlmJudgement j;
        j.output = o.output ? *o.output : Json{{"rain", true}};
        j.confidence = 0.9;
        j.rationale = "テスト用判定";
        if (!o.dropProposal) j.proposedRule = RuleProposal{"一致率 0.6 以上は雨", COND_RAIN, Json{{"rain", true}}, 0.8, std::nullopt};
        return j;
    };
}

RuleDraft rain_draft(const std::string& domain = "d") {
    RuleDraft d;
    d.domain = domain;
    d.description = "テスト";
    d.when = COND_RAIN;
    d.output = Json{{"rain", true}};
    return d;
}
} // namespace blackbox_engine_support
