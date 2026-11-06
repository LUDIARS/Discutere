#pragma once
// w3k_gate_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// w3-K: 情報ゲート (tests/flow/information-gate.test.ts)・密度のブラックボックス (density-blackbox.test.ts)・
// 自動学習クロール (learning-autocrawl.test.ts)・学習フロー (learning.test.ts)・slug の golden。
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>

#include "check.hpp"
#include "core/test_util.hpp"

#include "base/env.hpp"
#include "base/strings.hpp"
#include "core/index.hpp"
#include "flow/density_blackbox.hpp"
#include "flow/information_gate.hpp"
#include "flow/investigate.hpp"
#include "flow/learning.hpp"
#include "flow/learning_autocrawl.hpp"
#include "flow/user_voices/collect.hpp"
#include "ludus/economy_analyzer.hpp"

using namespace di;
using namespace di::flow;
namespace cs = di::crawler::sources;

namespace w3k_gate_support {

/** 与えた応答テキストを順に返す LLM (末尾は繰り返す)。 */
class ScriptLlm : public persona_engine::LLMClient {
public:
    explicit ScriptLlm(std::vector<std::string> texts, bool fail = false) : texts_(std::move(texts)), fail_(fail) {}
    persona_engine::LLMResult invoke(const persona_engine::LLMInvokeArgs& args) override {
        std::lock_guard lock(m_);
        last_system_ = args.system.value_or("");
        last_prompt_ = args.prompt;
        const std::size_t i = std::min(calls_, texts_.empty() ? 0 : texts_.size() - 1);
        ++calls_;
        if (fail_) return persona_engine::LLMResult::failure("down");
        return persona_engine::LLMResult::success(texts_.empty() ? std::string() : texts_[i]);
    }
    std::size_t calls() const { return calls_; }
    std::string last_system_, last_prompt_;

private:
    std::vector<std::string> texts_;
    bool fail_;
    std::size_t calls_ = 0;
    std::mutex m_;
};

std::string eval_json(const std::string& density, Json gaps = Json::array()) {
    return json_stringify(Json{{"density", density}, {"covered", Json::array()}, {"gaps", gaps}});
}

struct Growing {
    int n = 0;
    ListExternalVoicesFn list() {
        return [this](const std::vector<std::string>&, int) {
            return std::vector<ContextVoice>(static_cast<std::size_t>(n), ContextVoice{"c", "niconico", std::nullopt});
        };
    }
};

struct FakeCollect {
    explicit FakeCollect(Growing* v, int p = 5) : voices(v), per(p) {}
    Growing* voices;
    int per = 5;
    std::vector<std::pair<std::string, std::string>> calls;  // source, query
    CollectAndImportFn fn() {
        return [this](const CollectAndImportDeps& d) {
            calls.emplace_back(d.spec.source, d.spec.query.value_or(""));
            voices->n += per;
            return CollectAndImportResult{per, per};
        };
    }
};

InformationGateConfig base_config(int iterations = 2, int gaps = 2, std::string min = "moderate") {
    InformationGateConfig c;
    c.enabled = true;
    c.minDensity = std::move(min);
    c.maxLearnIterations = iterations;
    c.maxGapsPerIteration = gaps;
    return c;
}

std::vector<ContextVoice> voices_from(const Json& arr) {
    std::vector<ContextVoice> out;
    for (const auto& v : arr) out.push_back(context_voice_from_json(v));
    return out;
}

std::vector<FlowTag> tags_from(const Json& arr) {
    std::vector<FlowTag> out;
    for (const auto& t : arr) out.push_back(t.get<std::string>());
    return out;
}

} // namespace w3k_gate_support











// ── density-blackbox.test.ts ─────────────────────────────────────────────

namespace w3k_gate_support {

EvaluateArgs args_of(int n, const std::string& source = "niconico") {
    EvaluateArgs a;
    a.theme = "テーマ";
    a.voices.assign(static_cast<std::size_t>(n), ContextVoice{"1234567890", source, std::nullopt});
    return a;
}

EvaluateFn fake_evaluate(int* calls) {
    return [calls](const EvaluateArgs&) {
        ++*calls;
        InformationEvaluation e;
        e.density = "rich";
        e.covered = {"肯定", "否定"};
        e.proposedRule = Json{{"description", "8件以上は rich"}, {"when", Json{{"op", "cmp"}, {"feature", "voiceCount"}, {"cmp", ">="}, {"value", 8}}}};
        return e;
    };
}

} // namespace w3k_gate_support




// ── learning-autocrawl.test.ts ────────────────────────────────────────────

namespace w3k_gate_support {

std::vector<cs::ExternalUtterance> fake_items(int n) {
    std::vector<cs::ExternalUtterance> out;
    for (int i = 0; i < n; ++i) {
        cs::ExternalUtterance u;
        u.source = "niconico";
        u.nativeId = "n" + std::to_string(i);
        u.gameSlug = "x";
        u.threadKey = "t";
        u.content = "c";
        u.postedAt = 1;
        u.authorId = "anon";
        u.sourceUrl = "u";
        out.push_back(u);
    }
    return out;
}

ListExternalVoicesFn voices_of(int n) {
    return [n](const std::vector<std::string>&, int) { return std::vector<ContextVoice>(static_cast<std::size_t>(n)); };
}

} // namespace w3k_gate_support





// ── learning.test.ts ─────────────────────────────────────────────────────

namespace w3k_gate_support {

struct GuardSentiment : lib::sentiment_core::SentimentLlmClient {
    std::atomic<int> calls{0};
    lib::sentiment_core::SentimentLlmResponse invoke(const lib::sentiment_core::SentimentLlmRequest&) override {
        ++calls;
        throw std::runtime_error("LLM must NOT be called in learning flow");
    }
};

} // namespace w3k_gate_support
