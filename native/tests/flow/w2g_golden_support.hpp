#pragma once
// w2g_golden_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// w2-G: Node 実装の入出力 (native/tests/fixtures/flow/w2g_golden.json) と C++ の出力が同じであることを確かめる。
// 生成: npx tsx native/tests/golden/w2-G-flow.gen.ts
#include "flow/w2g_support.hpp"

#include <map>
#include <cmath>
#include <set>

#include "base/strings.hpp"
#include "flow/argument_graph.hpp"
#include "flow/debatability.hpp"
#include "flow/dialectic/exchange.hpp"
#include "flow/dialectic/exchange_prompt.hpp"
#include "flow/dialectic/goal.hpp"
#include "flow/dialectic/issue_voices.hpp"
#include "flow/dialectic/issues.hpp"
#include "flow/dialectic/outcome.hpp"
#include "flow/dialectic/position.hpp"
#include "flow/dialectic/position_stance.hpp"
#include "flow/dialectic/rule_check.hpp"
#include "flow/dialectic/scheduler.hpp"
#include "flow/dialectic/synthesis.hpp"
#include "flow/dialectic/tension.hpp"
#include "flow/dialectic/turn_prompt.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/effect_predict.hpp"
#include "flow/improvement_score.hpp"
#include "flow/mechanic_extract.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/paper_refine.hpp"
#include "flow/paper_reuse.hpp"
#include "flow/paper_voices.hpp"
#include "flow/persona_setup.hpp"
#include "flow/proxy/position_attribution.hpp"
#include "flow/round_summary.hpp"
#include "flow/spec_analyze.hpp"
#include "flow/user_voices/mix.hpp"
#include "flow/voice_cache.hpp"
#include "flow/vote.hpp"

using namespace di;
using namespace di::flow;
using w2g::FakeLLM;
using w2g::golden;
using w2g::Warns;

namespace w2g_golden_support {

const Json& G(const char* key) { return golden().at(key); }

std::vector<MechanicSummary> mechs_from(const Json& a) {
    std::vector<MechanicSummary> out;
    for (const auto& m : a) out.push_back(mechanic_summary_from_json(m));
    return out;
}

Json mechs_json(const std::vector<MechanicSummary>& v) {
    Json a = Json::array();
    for (const auto& m : v) a.push_back(to_json(m));
    return a;
}

std::vector<RoundSummary> rounds_from(const Json& a) {
    std::vector<RoundSummary> out;
    for (const auto& r : a) out.push_back({r["round"].get<int>(), r["summary"], w2g::strings(r["aufhebung"])});
    return out;
}

FlowPersona persona_from(const Json& j) { return flow_persona_from_json(j); }

Json opt_json(const std::optional<std::string>& v) { return v ? Json(*v) : Json(nullptr); }

template <class T>
Json list_json(const std::vector<T>& v) {
    Json a = Json::array();
    for (const auto& x : v) a.push_back(to_json(x));
    return a;
}

/** 応答例から呼び出し・警告・結果を比べる共通部。 */
struct LlmCase {
    const Json& c;
    FakeLLM llm;
    Warns warns;
    explicit LlmCase(const Json& cs) : c(cs), llm(cs["responses"]) {}
    void check(const Json& result, const char* label) {
        if (!w2g::same(llm.calls, c["calls"]))
            throw test::Failure(std::string(label) + " calls\n  actual:   " + llm.calls.dump() + "\n  expected: " + c["calls"].dump());
        if (!w2g::same(warns.json(), c["warns"]))
            throw test::Failure(std::string(label) + " warns\n  actual:   " + warns.json().dump() + "\n  expected: " + c["warns"].dump());
        if (!w2g::same(result, c["result"]))
            throw test::Failure(std::string(label) + " result\n  actual:   " + result.dump() + "\n  expected: " + c["result"].dump());
    }
};

} // namespace w2g_golden_support














namespace w2g_golden_support {

using namespace di::flow::dialectic;

const Json& IN(const char* key) { return G("inputs").at(key); }

Json position_json(const PositionRecord& p) {
    Json o = Json::object();
    o["id"] = p.id;
    o["issueId"] = p.issueId;
    o["personaId"] = p.personaId;
    o["stance"] = p.stance;
    o["claim"] = p.claim;
    o["qualifier"] = opt_json(p.qualifier);
    o["grounds"] = list_json(p.grounds);
    o["values"] = p.values;
    return o;
}

Json node_json(const ArgumentNode& n) {
    Json o = Json::object();
    o["utteranceId"] = n.utteranceId;
    o["side"] = n.side;
    o["personaId"] = n.personaId;
    o["groundId"] = opt_json(n.groundId);
    o["data"] = n.data;
    o["text"] = n.text;
    if (n.warrant) o["warrant"] = *n.warrant;
    if (n.kind) o["kind"] = *n.kind;
    o["id"] = n.id;
    o["parentId"] = opt_json(n.parentId);
    o["depth"] = n.depth;
    o["status"] = n.status;
    return o;
}

Json nodes_json(const std::vector<ArgumentNode>& v) {
    Json a = Json::array();
    for (const auto& n : v) a.push_back(node_json(n));
    return a;
}

Json synthesis_record_json(const SynthesisRecord& s) {
    Json o = Json::object();
    o["tensionId"] = s.tensionId;
    o["preserves"] = s.preserves;
    o["negates"] = s.negates;
    o["elevates"] = opt_json(s.elevates);
    o["kind"] = s.kind;
    o["text"] = s.text;
    o["ratification"] = list_json(s.ratification);
    o["status"] = s.status;
    o["revision"] = s.revision;
    o["id"] = "<id>";
    return o;
}

std::map<std::string, std::string> names_map() {
    std::map<std::string, std::string> m;
    for (auto it = IN("names").begin(); it != IN("names").end(); ++it) m[it.key()] = it.value();
    return m;
}

FlowPersona P(int i) { return persona_from(IN("personas")[static_cast<std::size_t>(i)]); }

} // namespace w2g_golden_support
