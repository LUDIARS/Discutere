#pragma once
// paper_review_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// w3-O: ペーパーレビューの共有コア (src/flow/paper-review.ts / paper-review-enrich.ts) のテスト。
//   - golden: Node 実装の入出力 (native/tests/fixtures/w3o/golden.json) と同じ出力・同じ LLM プロンプトになること。
//     生成: npx tsx native/tests/golden/w3-O-golden.gen.ts
//   - 移した TS テスト: tests/flow/paper-review.test.ts / paper-review-discussion-mechanics.test.ts /
//     debatability.test.ts (buildPaperDraft の `# 論点` 前倒し) / anatomia-refine.test.ts (mergeMechanics)。
//   - 境界: LLM 応答 JSON の上限 (ちょうど / +1 / 壊れた / 巨大)。
#include "w3o/w3o_support.hpp"

#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "flow/effect_predict.hpp"
#include "flow/paper_blocks.hpp"
#include "flow/paper_review.hpp"
#include "flow/paper_review_enrich.hpp"
#include "persona_engine/llm/mock.hpp"

using namespace di;
using namespace di::flow;
using w3o::FakeLLM;
using w3o::golden;
using w3o::TempDir;
using w3o::Warns;

namespace paper_review_support {

const Json& G(const char* key) { return golden().at(key); }

std::vector<MechanicSummary> mechs_from(const Json& a) {
    std::vector<MechanicSummary> out;
    if (a.is_array())
        for (const auto& m : a) out.push_back(mechanic_summary_from_json(m));
    return out;
}

Json mechs_json(const std::vector<MechanicSummary>& v) {
    Json a = Json::array();
    for (const auto& m : v) a.push_back(to_json(m));
    return a;
}

std::vector<ContextVoice> voices_from(const Json& a) {
    std::vector<ContextVoice> out;
    if (a.is_array())
        for (const auto& v : a) out.push_back(context_voice_from_json(v));
    return out;
}

PaperFixedFields fields_from(const Json& j) {
    return {json_string(j, "gameTitle"), json_string(j, "discussionTheme"), json_string(j, "discussionContent"),
            json_string(j, "mechanicsContext"), json_string(j, "themeSupplement")};
}

std::optional<std::string> opt(const Json& o, const char* key) {
    if (!o.is_object() || !o.contains(key) || o[key].is_null()) return std::nullopt;
    return o[key].get<std::string>();
}

PaperReviewInfo info_from(const Json& j) {
    PaperReviewInfo info;
    info.voiceCount = static_cast<int>(json_int(j, "voiceCount"));
    info.countCapped = json_bool(j, "countCapped");
    for (const auto& s : json_array(j, "samples")) info.samples.push_back({json_string(s, "content"), json_string(s, "source")});
    if (j.contains("debatability")) {
        const auto& d = j["debatability"];
        DebatabilityResult r;
        r.debatable = json_bool(d, "debatable");
        r.degraded = json_bool(d, "degraded");
        r.message = json_string(d, "message");
        if (d.contains("recommendation")) r.recommendation = FlowRecommendation{json_string(d["recommendation"], "flow"), json_string(d["recommendation"], "reason")};
        info.debatability = r;
    }
    return info;
}

ListExternalVoicesFn fixed_voices(const Json& voices, Json* calls = nullptr) {
    return [voices, calls](const std::vector<std::string>& terms, int limit) {
        if (calls) calls->push_back(Json{{"terms", terms}, {"limit", limit}});
        return voices_from(voices);
    };
}

} // namespace paper_review_support















// ── 境界: LLM 応答 JSON の上限 ──────────────────────────────────────────────

namespace paper_review_support {

/** {"changeSummary":"<pad>"} がちょうど n バイトになる応答。 */
std::string edit_response_of_size(std::size_t n) {
    const std::string head = R"({"changeSummary":")";
    const std::string tail = R"("})";
    return head + std::string(n - head.size() - tail.size(), 'a') + tail;
}

} // namespace paper_review_support
