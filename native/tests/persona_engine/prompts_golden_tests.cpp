// プロンプト組み立て・seed・単価推定が Node 実装と 1 文字も違わないことを確かめる
// (golden: native/tests/golden/w1-B-persona_engine.gen.ts)。tests/persona-engine/facilitator.test.ts の
// extractJsonObject の観点もここに移す。
#include "check.hpp"

#include "base/fsutil.hpp"
#include "persona_engine/engine/prompt_builder.hpp"
#include "persona_engine/facilitator/prompts.hpp"
#include "persona_engine/llm/pricing.hpp"
#include "persona_engine/llm/tier_routing.hpp"
#include "persona_engine/seeds/personas.hpp"
#include "persona_engine/seeds/rules.hpp"
#include "persona_engine/worker_pool/debate_rules.hpp"
#include "persona_engine/worker_pool/persona_prompts.hpp"

using namespace di;
using namespace di::persona_engine;

namespace {
Json load(const std::string& name) {
    auto text = fs::read_file(test::fixtures_dir() + "/persona_engine/" + name);
    if (!text) throw std::runtime_error("fixture missing: " + name);
    return parse_json_or_throw(*text, {8u << 20, 1'000'000, 64});
}
std::optional<std::string> opt_str(const Json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}
std::optional<std::int64_t> opt_int(const Json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return std::nullopt;
    return it->get<std::int64_t>();
}
void check_rule_seed(const RuleSeed& s, const Json& j) {
    CHECK_EQ(s.id, j["id"].get<std::string>());
    CHECK(s.description == opt_str(j, "description"));
    CHECK_EQ(s.trigger_type, j["trigger_type"].get<std::string>());
    CHECK(s.tick_sec == opt_int(j, "tick_sec"));
    CHECK(s.event_kind == opt_str(j, "event_kind"));
    CHECK_EQ(s.instructions, j["instructions"].get<std::string>());
    CHECK(s.target == opt_str(j, "target"));
    CHECK(s.cooldown_sec == opt_int(j, "cooldown_sec"));
}
std::vector<std::string> strings(const Json& j) { return j.get<std::vector<std::string>>(); }

/** fixture の scenario を返し、呼び出しを記録する偽の provider。 */
class FakeProvider : public DiscussionContextProvider {
public:
    explicit FakeProvider(const Json& s) : s_(s) {}
    Json calls = Json::array();
    std::vector<ContextHypothesis> list_active_hypotheses(const std::string& ws, int limit) override {
        calls.push_back(Json::array({"hyp", ws, limit}));
        return {{"h1", "天井は必要", "proposed", std::string("g1"), 10}, {"h2", "不要", "proposed", std::nullopt, 9}};
    }
    std::vector<ContextGap> list_recent_gaps(const std::string& ws, int limit) override {
        calls.push_back(Json::array({"gaps", ws, limit}));
        std::vector<ContextGap> out;
        for (const auto& g : s_["gaps"]) {
            ContextGap c;
            c.id = g["id"].get<std::string>();
            c.title = g["title"].get<std::string>();
            c.description = opt_str(g, "description");
            c.status = g["status"].get<std::string>();
            c.gapIn = opt_str(g, "gapIn");
            c.expectedAffect = opt_str(g, "expectedAffect");
            c.observedAffect = opt_str(g, "observedAffect");
            c.hypothesisIds = strings(g["hypothesisIds"]);
            c.updatedAt = g["updatedAt"].get<std::int64_t>();
            out.push_back(c);
        }
        return out;
    }
    std::vector<ContextUtterance> list_recent_utterances(const std::string& ws, const std::string& sid, int limit) override {
        calls.push_back(Json::array({"utt", ws, sid, limit}));
        return {{"u1", sid, "人間です \"引用\"", 100}};
    }
    std::vector<ContextExternalVoice> list_relevant_external_voices(const std::string& ws, const std::vector<std::string>& terms,
                                                                    int limit) override {
        calls.push_back(Json::array({"voices", ws, terms, limit}));
        if (!s_["voices"].get<bool>()) return {};
        return {{"v1", "天井ありがたい", "youtube", std::string("https://example.com/v"), "論者#a1b2", 0.75},
                {"v2", "x", "steam", std::nullopt, "外部の声", 1}};
    }
    std::vector<std::string> list_facilitator_directives(const std::string& ws, const std::string& gap, int limit) override {
        calls.push_back(Json::array({"dir", ws, gap, limit}));
        return strings(s_["directives"]);
    }
    std::string propose_hypothesis(const ProposeHypothesisInput&) override { return "x"; }
    std::string post_utterance(const PostUtteranceInput&) override { return "x"; }
    std::optional<std::string> find_active_discussion_session(const std::string& ws) override {
        calls.push_back(Json::array({"active", ws}));
        return opt_str(s_, "active");
    }

private:
    const Json& s_;
};

std::vector<facilitator::RecentUtterance> recent_of(const Json& j) {
    std::vector<facilitator::RecentUtterance> out;
    for (const auto& u : j) out.push_back({u["speaker"].get<std::string>(), u["content"].get<std::string>()});
    return out;
}
} // namespace

TEST_CASE("buildPrompt matches Node golden (system / user / provider calls)") {
    for (const auto& c : load("prompt_builder.json")) {
        const Json& s = c["scenario"];
        PersonaRow persona{"sceptic", "懐疑者", "問田 鋭", "弱点を探す役", R"(["疑り深い",3,"鋭い"])", "鋭く", "", "[]", 1, 2};
        RuleRow rule;
        rule.id = "refute-cold";
        rule.description = "説明";
        rule.trigger_type = "tick";
        rule.tick_sec = 300;
        rule.conditions = "[]";
        rule.instructions = "指示1\n\n指示2";
        rule.target = "sceptic";
        rule.cooldown_sec = 180;
        if (s.contains("rule") && s["rule"].contains("description")) rule.description = opt_str(s["rule"], "description");
        if (s.contains("persona") && s["persona"].contains("traits")) persona.traits = s["persona"]["traits"].get<std::string>();
        FakeProvider provider(s);
        engine::BuildPromptArgs args;
        args.rule = &rule;
        args.persona = &persona;
        args.workspaceId = "ws";
        args.sessionId = opt_str(s, "sessionId");
        args.contextProvider = &provider;
        args.triggeredBy = "tick";
        const auto out = engine::build_prompt(args);
        const std::string name = s["name"].get<std::string>();
        if (out.system != c["system"].get<std::string>()) throw test::Failure(name + ": system differs");
        if (out.user != c["user"].get<std::string>())
            throw test::Failure(name + ": user differs\n--- c++\n" + out.user + "\n--- node\n" + c["user"].get<std::string>());
        CHECK_EQ(json_stringify(provider.calls), json_stringify(c["calls"]));
    }
}

TEST_CASE("facilitator prompts match Node golden") {
    const Json f = load("facilitator.json");
    for (const auto& c : f["expand"]) {
        auto p = facilitator::build_expand_prompt(c["topic"].get<std::string>(), strings(c["existing"]), recent_of(c["recent"]));
        CHECK_EQ(p.system, c["out"]["system"].get<std::string>());
        CHECK_EQ(p.user, c["out"]["user"].get<std::string>());
    }
    for (const auto& c : f["aufhebung"]) {
        auto p = facilitator::build_aufhebung_judge_prompt(c["topic"].get<std::string>(), recent_of(c["recent"]), strings(c["stocked"]));
        CHECK_EQ(p.system, c["out"]["system"].get<std::string>());
        CHECK_EQ(p.user, c["out"]["user"].get<std::string>());
    }
    for (const auto& c : f["converge"]) {
        std::optional<std::vector<std::string>> auf, top;
        if (c.contains("auf")) auf = strings(c["auf"]);
        if (c.contains("top")) top = strings(c["top"]);
        auto p = facilitator::build_converge_prompt(c["topic"].get<std::string>(), recent_of(c["recent"]), auf, top);
        CHECK_EQ(p.system, c["out"]["system"].get<std::string>());
        CHECK_EQ(p.user, c["out"]["user"].get<std::string>());
    }
}

TEST_CASE("extractJsonObject: first { to last }, errors, and size limit") {
    auto j = facilitator::extract_json_object("前置き {\"name\":\"a\",\"x\":{\"y\":1}} 後書き");
    CHECK_EQ(json_stringify(j), std::string(R"({"name":"a","x":{"y":1}})"));
    CHECK_THROWS(facilitator::extract_json_object("no json"));
    CHECK_THROWS(facilitator::extract_json_object("} {"));
    CHECK_THROWS(facilitator::extract_json_object("{broken}"));
    const auto limit = facilitator::llm_json_limits().max_bytes;
    // 上限ちょうどは読める / +1 は拒否。
    const std::string exact = "{\"a\":\"" + std::string(limit - 8, 'x') + "\"}";
    CHECK_EQ(exact.size(), limit);
    CHECK(facilitator::extract_json_object(exact).is_object());
    const std::string over = "{\"a\":\"" + std::string(limit - 7, 'x') + "\"}";
    CHECK_THROWS(facilitator::extract_json_object(over));
}

TEST_CASE("seeds / debate rules / worker prompts match Node golden") {
    const Json s = load("seeds.json");
    const auto& personas = seeds::DISCUSSION_PERSONA_SEEDS();
    CHECK_EQ(personas.size(), s["personas"].size());
    for (std::size_t i = 0; i < personas.size(); ++i)
        CHECK_EQ(json_stringify(to_json(personas[i])), json_stringify(s["personas"][i]));
    const auto& rules = seeds::DISCUSSION_RULE_SEEDS();
    CHECK_EQ(rules.size(), s["rules"].size());
    for (std::size_t i = 0; i < rules.size(); ++i) check_rule_seed(rules[i], s["rules"][i]);
    CHECK_EQ(seeds::SEED_RULE_IDS().size(), s["seedRuleIds"].size());
    for (const auto& id : s["seedRuleIds"]) CHECK(seeds::SEED_RULE_IDS().count(id.get<std::string>()) == 1);

    const auto& debate = worker_pool::DEBATE_RULE_SEEDS();
    CHECK_EQ(debate.size(), s["debateRules"].size());
    for (std::size_t i = 0; i < debate.size(); ++i) check_rule_seed(debate[i], s["debateRules"][i]);
    CHECK_EQ(worker_pool::RULE_INSTRUCTION_DEFAULTS().size(), s["ruleInstructionDefaults"].size());
    for (auto it = s["ruleInstructionDefaults"].begin(); it != s["ruleInstructionDefaults"].end(); ++it)
        CHECK_EQ(worker_pool::RULE_INSTRUCTION_DEFAULTS().at(it.key()), it.value().get<std::string>());

    const auto& workers = worker_pool::DEFAULT_WORKERS();
    CHECK_EQ(workers.size(), s["defaultWorkers"].size());
    for (std::size_t i = 0; i < workers.size(); ++i) {
        CHECK_EQ(workers[i].id, s["defaultWorkers"][i]["id"].get<std::string>());
        CHECK_EQ(workers[i].role, s["defaultWorkers"][i]["role"].get<std::string>());
        CHECK_EQ(workers[i].provider, s["defaultWorkers"][i]["provider"].get<std::string>());
        CHECK_EQ(workers[i].model, s["defaultWorkers"][i]["model"].get<std::string>());
    }
    for (auto it = s["roleGuidanceDefaults"].begin(); it != s["roleGuidanceDefaults"].end(); ++it)
        CHECK_EQ(worker_pool::ROLE_GUIDANCE_DEFAULTS().at(it.key()), it.value().get<std::string>());

    auto all = workers;
    all.push_back({"x-new", "謎役", "codex", "gpt-x"});
    const auto ps = worker_pool::build_worker_persona_seeds(all);
    for (std::size_t i = 0; i < ps.size(); ++i) CHECK_EQ(json_stringify(to_json(ps[i])), json_stringify(s["workerPersonaSeeds"][i]));
    for (std::size_t i = 0; i < all.size(); ++i)
        CHECK_EQ(worker_pool::build_standing_prompt(all[i]), s["standingPrompts"][i]["prompt"].get<std::string>());
}

TEST_CASE("role prompt resolver overrides guidance; blank override keeps default") {
    worker_pool::set_role_prompt_resolver([](const std::string& role) -> std::optional<std::string> {
        if (role == "否定派") return std::string("上書きされた否定派");
        if (role == "正論派") return std::string("   ");
        return std::nullopt;
    });
    const auto ps = worker_pool::build_worker_persona_seeds({{"con-opus", "否定派", "claude", "m"}, {"pro-opus", "正論派", "claude", "m"}});
    CHECK_EQ(ps[0].description, std::string("上書きされた否定派"));
    CHECK_EQ(ps[1].description, worker_pool::ROLE_GUIDANCE_DEFAULTS().at("正論派"));
    worker_pool::set_role_prompt_resolver(nullptr);
    const auto over = worker_pool::apply_rule_instruction_overrides(worker_pool::DEBATE_RULE_SEEDS(), [](const std::string& id) -> std::optional<std::string> {
        if (id == "tick-con-opus") return std::string("新しい指示");
        if (id == "tick-pro-opus") return std::string(" \n");
        return std::nullopt;
    });
    CHECK_EQ(over[2].instructions, std::string("新しい指示"));
    CHECK_EQ(over[3].instructions, worker_pool::DEBATE_RULE_SEEDS()[3].instructions);
    for (const auto& r : worker_pool::DEBATE_RULE_SEEDS())
        if (r.trigger_type == "tick") CHECK(r.tick_sec && *r.tick_sec > 0);  // tick rule は必ず tick_sec を持つ (回帰防止)
}

TEST_CASE("pricing / tier routing match Node golden") {
    const Json p = load("pricing.json");
    for (const auto& c : p["families"]) {
        const auto fam = model_family(opt_str(c, "model"));
        CHECK(fam == opt_str(c, "family"));
    }
    for (const auto& c : p["costs"]) {
        std::optional<TokenUsage> usage;
        if (c["usage"].is_object()) {
            TokenUsage u;
            u.input_tokens = json_opt_number(c["usage"], "input_tokens");
            u.output_tokens = json_opt_number(c["usage"], "output_tokens");
            u.cache_read_input_tokens = json_opt_number(c["usage"], "cache_read_input_tokens");
            u.cache_creation_input_tokens = json_opt_number(c["usage"], "cache_creation_input_tokens");
            usage = u;
        }
        const auto cost = estimate_cost_usd(opt_str(c, "model"), usage);
        if (c["cost"].is_null()) {
            CHECK(!cost);
        } else {
            CHECK(cost.has_value());
            CHECK_EQ(js_number_to_string(*cost), js_number_to_string(c["cost"].get<double>()));
        }
    }
    for (const auto& c : p["tiers"]) {
        lib::llm_gateway::RoutingInput in;
        in.kind = c["input"]["kind"].get<std::string>();
        if (c["input"].contains("inputTokens")) in.inputTokens = c["input"]["inputTokens"].get<double>();
        std::optional<TierModels> tiers;
        if (c["tiers"].is_object()) tiers = TierModels{opt_str(c["tiers"], "cheap"), opt_str(c["tiers"], "strong")};
        CHECK(resolve_tier_model(in, tiers) == opt_str(c, "model"));
    }
}

TEST_CASE("extract_topic_terms handles huge input linearly and drops short tokens") {
    ContextGap g;
    g.title = "ab";
    std::string desc;
    for (int i = 0; i < 20000; ++i) desc += "語句、x。";
    g.description = desc;
    const auto terms = engine::extract_topic_terms(g);
    CHECK_EQ(terms.size(), std::size_t(2));  // "ab" と "語句" (重複は 1 回、1 文字の x は落ちる)
    CHECK_EQ(terms[1], std::string("語句"));
}

DI_TEST_MAIN()
