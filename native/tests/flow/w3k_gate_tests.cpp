// w3-K: 情報ゲート (tests/flow/information-gate.test.ts)・密度のブラックボックス (density-blackbox.test.ts)・
// 自動学習クロール (learning-autocrawl.test.ts)・学習フロー (learning.test.ts)・slug の golden。
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

namespace {

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

} // namespace

// ── golden ────────────────────────────────────────────────────────────────

TEST_CASE("golden: to_slug / derive_slug / glab_game_slug") {
    for (const auto& c : test::load_fixture("w3k/slugs.json")) {
        const std::string in = c["input"].get<std::string>();
        CHECK_EQ(ludus::to_slug(in), c["toSlug"].get<std::string>());
        CHECK_EQ(derive_slug(in), c["deriveSlug"].get<std::string>());
        if (c["glabGameSlug"].get<std::string>() != user_voices::glab_game_slug(in))
            throw test::Failure("glab_game_slug mismatch for " + in + ": " + user_voices::glab_game_slug(in));
    }
}

TEST_CASE("golden: is_auto_crawl_source / resolve_auto_crawl_sources (reddit の認証情報なし)") {
    env::set_for_test("DISCUTERE_REDDIT_CLIENT_ID", std::nullopt);
    env::set_for_test("DISCUTERE_REDDIT_CLIENT_SECRET", std::nullopt);
    const Json f = test::load_fixture("w3k/auto_crawl_sources.json");
    for (const auto& p : f["isAutoCrawlSource"]) CHECK_EQ(is_auto_crawl_source(p[0].get<std::string>()), p[1].get<bool>());
    for (const auto& r : f["resolve"]) {
        std::vector<std::string> sources;
        for (const auto& s : r["sources"]) sources.push_back(s.get<std::string>());
        const auto key = r["key"].is_null() ? std::optional<std::string>() : r["key"].get<std::string>();
        Json out = Json::array();
        for (const auto& s : resolve_auto_crawl_sources(sources, key)) out.push_back(s);
        CHECK_EQ(json_stringify(out), json_stringify(r["out"]));
    }
    // reddit は認証情報があれば自動経路に乗る
    env::set_for_test("DISCUTERE_REDDIT_CLIENT_ID", "fake-id");
    env::set_for_test("DISCUTERE_REDDIT_CLIENT_SECRET", "fake-secret");
    CHECK((resolve_auto_crawl_sources({"reddit", "steam"}) == std::vector<std::string>{"reddit"}));
    CHECK_EQ(reddit_credentials_from_env()->userAgent, std::string("LUDIARS-Discutere/0.1 (external discussion crawler)"));
    env::set_for_test("DISCUTERE_REDDIT_CLIENT_ID", std::nullopt);
    env::set_for_test("DISCUTERE_REDDIT_CLIENT_SECRET", std::nullopt);
    CHECK(!reddit_credentials_from_env());
}

TEST_CASE("golden: 情報密度評価のプロンプトと応答の解釈 (1 バイト単位)") {
    const Json cases = test::load_fixture("w3k/information_gate.json");
    CHECK(cases.size() == 40);
    for (const auto& c : cases) {
        ScriptLlm llm({c["response"].get<std::string>()});
        EvaluateArgs a;
        a.theme = "周回設計";
        a.tags = tags_from(c["tags"]);
        a.voices = voices_from(c["voices"]);
        a.llm = &llm;
        const auto ev = evaluate_information_density(a);
        CHECK_EQ(llm.last_system_, c["system"].get<std::string>());
        CHECK_EQ(llm.last_prompt_, c["prompt"].get<std::string>());
        CHECK_EQ(json_stringify(to_json(ev)), json_stringify(c["evaluation"]));
    }
}

TEST_CASE("golden: density_features") {
    for (const auto& c : test::load_fixture("w3k/density_features.json")) {
        const auto f = density_features(voices_from(c["voices"]), tags_from(c["tags"]));
        CHECK_EQ(json_stringify(f), json_stringify(c["features"]));
    }
}

// ── information-gate.test.ts ─────────────────────────────────────────────

TEST_CASE("is_density ガード") {
    CHECK(is_density(Json("rich")));
    CHECK(!is_density(Json("MODERATE")));
    CHECK(!is_density(Json(3)));
}

TEST_CASE("evaluate: 正常 JSON / LLM 失敗 → 件数 fallback / 非 JSON → fallback / gaps 正規化") {
    {
        ScriptLlm llm({eval_json("rich")});
        EvaluateArgs a{"t", {}, {}, &llm, std::nullopt, {}};
        const auto ev = evaluate_information_density(a);
        CHECK_EQ(ev.density, std::string("rich"));
        CHECK(ev.gaps.empty());
    }
    {
        ScriptLlm llm({}, true);
        std::vector<std::string> warns;
        EvaluateArgs a{"t", {}, std::vector<ContextVoice>(4, ContextVoice{"c", "s", std::nullopt}), &llm, std::nullopt,
                       [&](const std::string& m) { warns.push_back(m); }};
        CHECK_EQ(evaluate_information_density(a).density, std::string("moderate"));
        CHECK(warns.size() == 1 && str::contains(warns[0], "down"));
    }
    {
        ScriptLlm llm({"JSON ではない文章"});
        EvaluateArgs a{"t", {}, {}, &llm, std::nullopt, {}};
        const auto ev = evaluate_information_density(a);
        CHECK_EQ(ev.density, std::string("sparse"));
        CHECK(ev.gaps.empty());
    }
    {
        ScriptLlm llm({eval_json("sparse", Json::array({Json{{"aspect", "否定的意見"}, {"reason", "偏り"}, {"query", ""}, {"source", "reddit"}},
                                                         Json{{"aspect", ""}}}))});
        EvaluateArgs a{"周回設計", {}, {}, &llm, std::nullopt, {}};
        const auto ev = evaluate_information_density(a);
        CHECK_EQ(ev.gaps.size(), std::size_t{1});
        CHECK_EQ(ev.gaps[0].aspect, std::string("否定的意見"));
        CHECK_EQ(ev.gaps[0].query, std::string("周回設計 否定的意見"));
        CHECK_EQ(ev.gaps[0].source.value_or(""), std::string("reddit"));
    }
    {
        // LLM 未設定・例外も fallback
        EvaluateArgs a{"t", {}, std::vector<ContextVoice>(9, ContextVoice{"c", "s", std::nullopt}), nullptr, std::nullopt, {}};
        CHECK_EQ(evaluate_information_density(a).density, std::string("rich"));
        struct Throwing : persona_engine::LLMClient {
            persona_engine::LLMResult invoke(const persona_engine::LLMInvokeArgs&) override { throw std::runtime_error("boom"); }
        } thrower;
        a.llm = &thrower;
        CHECK_EQ(evaluate_information_density(a).density, std::string("rich"));
    }
}

TEST_CASE("evaluate: LLM 応答の上限 (ちょうど・+1・巨大・深い入れ子・件数上限)") {
    // extract_json_object は 1MiB まで。ちょうど 1MiB の JSON は読める、+1 は fallback。
    const std::string head = "{\"density\":\"rich\",\"pad\":\"";
    const std::string tail = "\"}";
    const std::size_t limit = 1u << 20;
    std::string exact = head + std::string(limit - head.size() - tail.size(), 'a') + tail;
    CHECK_EQ(exact.size(), limit);
    {
        ScriptLlm llm({exact});
        EvaluateArgs a{"t", {}, {}, &llm, std::nullopt, {}};
        CHECK_EQ(evaluate_information_density(a).density, std::string("rich"));
    }
    {
        ScriptLlm llm({head + std::string(limit - head.size() - tail.size() + 1, 'a') + tail});
        EvaluateArgs a{"t", {}, {}, &llm, std::nullopt, {}};
        CHECK_EQ(evaluate_information_density(a).density, std::string("sparse"));
    }
    {
        ScriptLlm llm({std::string(32u << 20, '{')});
        EvaluateArgs a{"t", {}, {}, &llm, std::nullopt, {}};
        CHECK_EQ(evaluate_information_density(a).density, std::string("sparse"));
    }
    {
        std::string deep = "{\"density\":\"rich\",\"x\":" + std::string(5000, '[') + std::string(5000, ']') + "}";
        ScriptLlm llm({deep});
        EvaluateArgs a{"t", {}, {}, &llm, std::nullopt, {}};
        CHECK_EQ(evaluate_information_density(a).density, std::string("sparse"));
    }
    {
        Json gaps = Json::array();
        Json covered = Json::array();
        for (int i = 0; i < 500; ++i) {
            gaps.push_back(Json{{"aspect", "a" + std::to_string(i)}});
            covered.push_back("c");
        }
        ScriptLlm llm({json_stringify(Json{{"density", "sparse"}, {"covered", covered}, {"gaps", gaps}})});
        EvaluateArgs a{"t", {}, {}, &llm, std::nullopt, {}};
        const auto ev = evaluate_information_density(a);
        CHECK_EQ(ev.gaps.size(), kMaxGapItems);
        CHECK_EQ(ev.covered.size(), kMaxCoveredItems);
    }
}

TEST_CASE("gate: sparse → 1 回学習 → rich で十分") {
    Growing voices;
    FakeCollect fc{&voices};
    ScriptLlm llm({eval_json("sparse", Json::array({Json{{"aspect", "否定"}, {"reason", "r"}, {"query", "t 否定"}, {"source", "niconico"}}})),
                   eval_json("rich")});
    InformationGateDeps d;
    d.theme = "t";
    d.workspaceId = "knowledge";
    d.llm = &llm;
    d.listExternalVoices = voices.list();
    d.crawlSource = "niconico";
    d.config = base_config();
    d.collectAndImportFn = fc.fn();
    const auto r = run_information_gate(d);
    CHECK(r.ran && r.sufficient);
    CHECK_EQ(r.initialDensity, std::string("sparse"));
    CHECK_EQ(r.finalDensity, std::string("rich"));
    CHECK_EQ(r.crawls, 1);
    CHECK_EQ(r.importedTotal, 5);
    CHECK(fc.calls.size() == 1 && fc.calls[0].second == "t 否定");
    CHECK((r.crawledQueries == std::vector<std::string>{"niconico:t 否定"}));
    CHECK(str::starts_with(r.message, "情報充分 (密度 sparse→rich ≥ moderate / 学習 1 回・取込 5 件)"));
}

TEST_CASE("gate: 常に sparse → 反復上限 / 初回 moderate → 学習しない / LLM 全失敗でも材料が厚ければ通す") {
    {
        Growing voices;
        FakeCollect fc{&voices, 0};
        ScriptLlm llm({eval_json("sparse", Json::array({Json{{"aspect", "a"}, {"reason", "r"}, {"query", "q1"}}}))});
        InformationGateDeps d;
        d.theme = "t";
        d.llm = &llm;
        d.listExternalVoices = voices.list();
        d.crawlSource = "niconico";
        d.config = base_config(2, 1);
        d.collectAndImportFn = fc.fn();
        const auto r = run_information_gate(d);
        CHECK_EQ(r.crawls, 2);
        CHECK(!r.sufficient);
        CHECK_EQ(fc.calls.size(), std::size_t{2});
        CHECK(str::starts_with(r.message, "目標 (moderate) に未達"));
    }
    {
        Growing voices;
        FakeCollect fc{&voices};
        ScriptLlm llm({eval_json("moderate")});
        InformationGateDeps d;
        d.theme = "t";
        d.llm = &llm;
        d.listExternalVoices = voices.list();
        d.crawlSource = "niconico";
        d.config = base_config();
        d.collectAndImportFn = fc.fn();
        const auto r = run_information_gate(d);
        CHECK(r.crawls == 0 && r.sufficient && fc.calls.empty());
    }
    {
        Growing voices;
        voices.n = 10;
        FakeCollect fc{&voices};
        ScriptLlm llm({}, true);
        InformationGateDeps d;
        d.theme = "t";
        d.llm = &llm;
        d.listExternalVoices = voices.list();
        d.crawlSource = "niconico";
        d.config = base_config();
        d.collectAndImportFn = fc.fn();
        const auto r = run_information_gate(d);
        CHECK_EQ(r.finalDensity, std::string("rich"));
        CHECK(r.sufficient && r.crawls == 0 && fc.calls.empty());
    }
}

TEST_CASE("gate: gaps 空ならテーマで学習・複数ソース横断・crawlSource へのフォールバック・失敗の継続") {
    {
        Growing voices;
        FakeCollect fc{&voices, 0};
        ScriptLlm llm({eval_json("sparse")});
        InformationGateDeps d;
        d.theme = "テーマX";
        d.llm = &llm;
        d.listExternalVoices = voices.list();
        d.crawlSource = "niconico";
        d.config = base_config(1, 2);
        d.collectAndImportFn = fc.fn();
        const auto r = run_information_gate(d);
        CHECK_EQ(r.crawls, 1);
        CHECK(fc.calls.size() == 1 && fc.calls[0].second == "テーマX");
    }
    {
        Growing voices;
        FakeCollect fc{&voices};
        ScriptLlm llm({eval_json("sparse", Json::array({Json{{"aspect", "否定"}, {"reason", "r"}, {"query", "t 否定"}}})), eval_json("rich")});
        InformationGateDeps d;
        d.theme = "t";
        d.llm = &llm;
        d.listExternalVoices = voices.list();
        d.crawlSources = {"niconico", "youtube"};
        d.crawlSource = "steam";  // crawlSources が優先
        d.config = base_config();
        d.collectAndImportFn = fc.fn();
        const auto r = run_information_gate(d);
        CHECK_EQ(r.crawls, 1);
        CHECK_EQ(fc.calls.size(), std::size_t{2});
        CHECK(fc.calls[0].first == "niconico" && fc.calls[1].first == "youtube");
        CHECK(fc.calls[0].second == "t 否定" && fc.calls[1].second == "t 否定");
        CHECK_EQ(r.importedTotal, 10);
    }
    {
        Growing voices;
        std::vector<std::string> warns;
        ScriptLlm llm({eval_json("sparse", Json::array({Json{{"aspect", "a"}, {"query", "q"}}})), eval_json("rich")});
        InformationGateDeps d;
        d.theme = "t";
        d.llm = &llm;
        d.listExternalVoices = [](const std::vector<std::string>&, int) -> std::vector<ContextVoice> { throw std::runtime_error("db"); };
        d.crawlSources = {"niconico", "youtube"};
        d.config = base_config();
        d.warn = [&](const std::string& m) { warns.push_back(m); };
        d.collectAndImportFn = [](const CollectAndImportDeps& c) -> CollectAndImportResult {
            if (c.spec.source == "youtube") throw std::runtime_error("yt boom");
            return {3, 3};
        };
        const auto r = run_information_gate(d);
        CHECK_EQ(r.importedTotal, 3);
        CHECK((r.crawledQueries == std::vector<std::string>{"niconico:q"}));
        CHECK(warns.size() == 1 && warns[0] == "学習クロール失敗 (source=youtube query=\"q\"): yt boom");
        CHECK(r.sufficient);
    }
    {
        // minDensity が不正なら学習せず sufficient=false (TS の DENSITY_RANK[x] = undefined と同じ)
        Growing voices;
        FakeCollect fc{&voices};
        ScriptLlm llm({eval_json("sparse")});
        InformationGateDeps d;
        d.theme = "t";
        d.llm = &llm;
        d.crawlSource = "niconico";
        d.config = base_config(2, 2, "bogus");
        d.collectAndImportFn = fc.fn();
        const auto r = run_information_gate(d);
        CHECK(!r.sufficient && r.crawls == 0 && fc.calls.empty());
    }
}

// ── density-blackbox.test.ts ─────────────────────────────────────────────

namespace {

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

} // namespace

TEST_CASE("density_features: フラット特徴量") {
    auto voices = args_of(3).voices;
    for (const auto& v : args_of(2, "youtube").voices) voices.push_back(v);
    const auto f = density_features(voices, {"機密"});
    CHECK_EQ(f["voiceCount"].get<int>(), 5);
    CHECK_EQ(f["sourceKinds"].get<int>(), 2);
    CHECK_EQ(f["avgLen"].get<int>(), 10);
    CHECK_EQ(f["tagCount"].get<int>(), 1);
    CHECK_EQ(density_features({}, {})["avgLen"].get<int>(), 0);
}

TEST_CASE("blackbox: LLM 経路は完全な評価を透過・影評価で trial へ・条件不一致は LLM・不正な提案は登録しない") {
    {
        auto bb = lib::blackbox::make_memory_blackbox();
        int calls = 0;
        auto evaluate = make_blackbox_density_evaluator(bb, fake_evaluate(&calls));
        const auto ev = evaluate(args_of(10));
        CHECK_EQ(ev.density, std::string("rich"));
        CHECK((ev.covered == std::vector<std::string>{"肯定", "否定"}));
        CHECK_EQ(calls, 1);
        const auto rules = bb.rules->list_by_domain(DOMAIN_DENSITY);
        CHECK(rules.size() == 1 && rules[0].state == "candidate");
    }
    {
        auto bb = lib::blackbox::make_memory_blackbox();
        int calls = 0;
        auto evaluate = make_blackbox_density_evaluator(bb, fake_evaluate(&calls));
        for (int i = 0; i < 4; ++i) evaluate(args_of(10));
        CHECK_EQ(bb.rules->list_by_domain(DOMAIN_DENSITY)[0].state, std::string("trial"));
        CHECK_EQ(calls, 4);
        const auto ev = evaluate(args_of(12));
        CHECK_EQ(calls, 4);
        CHECK_EQ(ev.density, std::string("rich"));
        CHECK(ev.gaps.empty());
        evaluate(args_of(2));  // when 不一致
        CHECK_EQ(calls, 5);
    }
    {
        auto bb = lib::blackbox::make_memory_blackbox();
        auto evaluate = make_blackbox_density_evaluator(bb, [](const EvaluateArgs&) {
            InformationEvaluation e;
            e.density = "moderate";
            e.proposedRule = Json{{"when", Json{{"op", "bogus"}}}};
            return e;
        });
        CHECK_EQ(evaluate(args_of(4)).density, std::string("moderate"));
        CHECK(bb.rules->list_by_domain(DOMAIN_DENSITY).empty());
    }
    // parse_proposed_rule の境界
    CHECK(!parse_proposed_rule(std::nullopt, "rich"));
    CHECK(!parse_proposed_rule(Json("x"), "rich"));
    CHECK(!parse_proposed_rule(Json::array(), "rich"));
    const auto p = parse_proposed_rule(Json{{"when", Json{{"op", "cmp"}, {"feature", "voiceCount"}, {"cmp", ">="}, {"value", 3}}}}, "moderate");
    CHECK(p && p->description == "密度 moderate の閾値ルール" && p->confidence == 0.7 && p->output["density"] == "moderate");
    // 深すぎる条件は validate_condition が拒否
    Json deep = Json{{"op", "cmp"}, {"feature", "voiceCount"}, {"cmp", ">="}, {"value", 1}};
    for (int i = 0; i < 20; ++i) deep = Json{{"op", "not"}, {"clause", deep}};
    CHECK(!parse_proposed_rule(Json{{"when", deep}}, "rich"));
}

TEST_CASE("blackbox: ルールの output が不正なら sparse (安全側)") {
    auto bb = lib::blackbox::make_memory_blackbox();
    lib::blackbox::RuleDraft draft;
    draft.domain = DOMAIN_DENSITY;
    draft.description = "壊れた出力";
    draft.when = Json{{"op", "cmp"}, {"feature", "voiceCount"}, {"cmp", ">="}, {"value", 1}};
    draft.output = Json{{"density", "huge"}};
    bb.engine->add_rule(draft);  // manual は trial から
    int calls = 0;
    auto evaluate = make_blackbox_density_evaluator(bb, fake_evaluate(&calls));
    CHECK_EQ(evaluate(args_of(3)).density, std::string("sparse"));
    CHECK_EQ(calls, 0);
}

// ── learning-autocrawl.test.ts ────────────────────────────────────────────

namespace {

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

} // namespace

TEST_CASE("autocrawl: ヘルパ") {
    CHECK(is_auto_crawl_source("niconico") && is_auto_crawl_source("reddit") && !is_auto_crawl_source("x"));
    CHECK_EQ(derive_slug("Hello World"), std::string("hello-world"));
    CHECK_EQ(derive_slug("ローグライト"), std::string("ローグライト"));
    CHECK_EQ(derive_slug("   "), std::string("discussion"));
    CHECK_EQ(count_learning_voices("t", voices_of(4), 3), 4);
    CHECK_EQ(count_learning_voices("t", {}, 3), 0);
    CHECK_EQ(count_learning_voices("t", [](const std::vector<std::string>&, int) -> std::vector<ContextVoice> { throw std::runtime_error("x"); }, 3), 0);
    CollectorContext ctx;
    ctx.theme = "テーマ";
    ctx.spec.query = "  ";
    CHECK_EQ(resolve_query(ctx), std::string("テーマ"));
    ctx.spec.query = " q ";
    CHECK_EQ(resolve_query(ctx), std::string("q"));
}

TEST_CASE("autocrawl: 充足 → skip / 不足 → クロール → 取込 / 0 件 / listExternalVoices 未指定 / 未対応ソース") {
    {
        bool called = false;
        EnsureLearningDataDeps d;
        d.theme = "ローグライト";
        d.slug = "rogue";
        d.workspaceId = "knowledge";
        d.spec.source = "niconico";
        d.minVoices = 3;
        d.listExternalVoices = voices_of(5);
        d.collectors["niconico"] = [&](const CollectorContext&) {
            called = true;
            return std::vector<cs::ExternalUtterance>{};
        };
        d.importItems = [](core::Core*, const std::vector<cs::ExternalUtterance>&, const std::string&) { return 0; };
        const auto r = ensure_learning_data(d);
        CHECK(r.skipped && !r.crawled && r.voicesBefore == 5 && !called);
        CHECK_EQ(r.message, std::string("学習データ充分 (外部の声 5 件 ≥ 3)"));
    }
    {
        std::string saw;
        int imported = 0;
        EnsureLearningDataDeps d;
        d.theme = "周回設計";
        d.slug = "shuukai";
        d.spec.source = "niconico";
        d.minVoices = 3;
        d.listExternalVoices = voices_of(1);
        d.collectors["niconico"] = [&](const CollectorContext& ctx) {
            saw = resolve_query(ctx);
            CHECK_EQ(ctx.gameSlug, std::string("shuukai"));
            CHECK_EQ(ctx.maxItems, 200);
            return fake_items(7);
        };
        d.importItems = [&](core::Core*, const std::vector<cs::ExternalUtterance>& items, const std::string&) {
            imported = static_cast<int>(items.size());
            return imported;
        };
        const auto r = ensure_learning_data(d);
        CHECK(!r.skipped && r.crawled && r.imported == 7 && imported == 7);
        CHECK_EQ(saw, std::string("周回設計"));
        CHECK_EQ(r.message, std::string("niconico で 7 件取得 → 7 件を学習データに追加"));
    }
    {
        EnsureLearningDataDeps d;
        d.theme = "無人気テーマ";
        d.spec.source = "niconico";
        d.minVoices = 3;
        d.listExternalVoices = voices_of(0);
        d.collectors["niconico"] = [](const CollectorContext&) { return std::vector<cs::ExternalUtterance>{}; };
        d.importItems = [](core::Core*, const std::vector<cs::ExternalUtterance>&, const std::string&) { return 0; };
        const auto r = ensure_learning_data(d);
        CHECK(!r.crawled && !r.skipped && r.imported == 0);
    }
    {
        EnsureLearningDataDeps d;
        d.theme = "t";
        d.spec.source = "niconico";
        d.minVoices = 3;
        d.collectors["niconico"] = [](const CollectorContext&) { return fake_items(2); };
        d.importItems = [](core::Core*, const std::vector<cs::ExternalUtterance>& items, const std::string&) { return static_cast<int>(items.size()); };
        const auto r = ensure_learning_data(d);
        CHECK(r.voicesBefore == 0 && r.crawled && r.imported == 2);
    }
    {
        std::vector<std::string> warns;
        EnsureLearningDataDeps d;
        d.theme = "t";
        d.spec.source = "bogus";
        d.minVoices = 3;
        d.warn = [&](const std::string& m) { warns.push_back(m); };
        const auto r = ensure_learning_data(d);
        CHECK_EQ(r.message, std::string("未対応のソース: bogus"));
        CHECK(warns.size() == 1);
    }
}

TEST_CASE("autocrawl: 既定 collector の入力検査 (steam は appId、website は URL、youtube はキー、reddit は認証情報)") {
    const auto& d = default_collectors();
    CollectorContext ctx;
    ctx.theme = "t";
    ctx.gameSlug = "g";
    ctx.spec.source = "steam";
    CHECK_THROWS(d.at("steam")(ctx));
    ctx.spec.appId = 0;
    CHECK_THROWS(d.at("steam")(ctx));
    ctx.spec.source = "website";
    CHECK_THROWS(d.at("website")(ctx));
    ctx.spec.urls = std::vector<std::string>{};
    CHECK_THROWS(d.at("website")(ctx));
    ctx.spec.source = "youtube";
    CHECK_THROWS(d.at("youtube")(ctx));
    ctx.youtubeApiKey = "";
    CHECK_THROWS(d.at("youtube")(ctx));
    env::set_for_test("DISCUTERE_REDDIT_CLIENT_ID", std::nullopt);
    ctx.spec.source = "reddit";
    CHECK_THROWS(d.at("reddit")(ctx));
    // website は利用者の URL: 私設アドレスは取得しない (SSRF 対策のまま)。失敗は skip されて 0 件。
    ctx.spec.source = "website";
    ctx.spec.urls = std::vector<std::string>{"http://127.0.0.1:1/x", "http://169.254.169.254/latest/meta-data/", "http://10.0.0.1/"};
    std::vector<cs::ExternalUtterance> got;
    try {
        got = d.at("website")(ctx);
    } catch (...) {
    }
    CHECK(got.empty());
}

TEST_CASE("collect_and_import: 未対応ソースは 0 件・collector の例外はそのまま投げる・0 件は取込を呼ばない") {
    CollectAndImportDeps d;
    d.theme = "t";
    d.spec.source = "bogus";
    std::vector<std::string> warns;
    d.warn = [&](const std::string& m) { warns.push_back(m); };
    const auto r = collect_and_import(d);
    CHECK(r.collected == 0 && r.imported == 0 && warns.size() == 1);
    d.spec.source = "niconico";
    d.collectors["niconico"] = [](const CollectorContext&) -> std::vector<cs::ExternalUtterance> { throw std::runtime_error("net"); };
    CHECK_THROWS(collect_and_import(d));
    bool import_called = false;
    d.collectors["niconico"] = [](const CollectorContext&) { return std::vector<cs::ExternalUtterance>{}; };
    d.importItems = [&](core::Core*, const std::vector<cs::ExternalUtterance>&, const std::string&) {
        import_called = true;
        return 0;
    };
    collect_and_import(d);
    CHECK(!import_called);
}

// ── learning.test.ts ─────────────────────────────────────────────────────

namespace {

struct GuardSentiment : lib::sentiment_core::SentimentLlmClient {
    std::atomic<int> calls{0};
    lib::sentiment_core::SentimentLlmResponse invoke(const lib::sentiment_core::SentimentLlmRequest&) override {
        ++calls;
        throw std::runtime_error("LLM must NOT be called in learning flow");
    }
};

} // namespace

TEST_CASE("learning: 意見/メカニクス記録 + 極性・LLM を呼ばない・investigate が読める・個人データ非保管・日本語 slug・自動収集") {
    test::TempDir dir;
    const std::string games_dir = dir.file("games");
    std::filesystem::create_directories(games_dir);
    auto core = core::create_core(dir.file("db.sqlite"), dir.file("events.jsonl"));
    auto guard = std::make_shared<GuardSentiment>();
    crawler::sentiment::CascadeClients clients;
    clients.main = guard;

    LearningFlowDeps d;
    d.core = core.get();
    d.slug = "test-game";
    d.gamesDir = games_dir;
    d.sentimentClients = clients;
    d.opinions = {LearningOpinion{"このゲームは面白い、神ゲー", std::nullopt}, LearningOpinion{"つまらない、最悪だった", std::nullopt},
                  LearningOpinion{"   ", std::nullopt}};
    GameMechanicEntry m;
    m.name = "連鎖コンボ";
    m.description = "連鎖でスコアが伸びる";
    m.intended_affect = "爽快感";
    m.intended_valence = "positive";
    m.intended_aspects = std::vector<std::string>{"fun"};
    m.intended_emotions = std::vector<std::string>{"joy"};
    d.mechanics = {m};
    d.log = [](const std::string&) {};
    const auto result = run_learning_flow("Test Game", "Test Game の面白さ", d);
    CHECK_EQ(result.mechanicsRecorded, 1);
    CHECK_EQ(result.opinionsRecorded, 2);
    CHECK_EQ(result.polarityBreakdown["positive"].get<int>(), 1);
    CHECK_EQ(result.polarityBreakdown["negative"].get<int>(), 1);
    CHECK_EQ(json_stringify(result.polarityBreakdown), std::string(R"({"positive":1,"negative":1,"neutral":0})"));
    CHECK_EQ(guard->calls.load(), 0);

    InvestigateArgs ia;
    ia.theme = "Test Game";
    ia.gamesDir = games_dir;
    const auto inv = investigate_theme(ia);
    bool found = false;
    for (const auto& mm : inv.mechanics)
        if (mm.name == "連鎖コンボ") {
            found = true;
            CHECK_EQ(mm.intended_affect.value_or(""), std::string("爽快感"));
            CHECK((mm.intended_aspects == std::vector<std::string>{"fun"}));
        }
    CHECK(found);

    const auto rows = core->raw().all("SELECT id, speaker_id, raw_content FROM utterances WHERE speaker_id LIKE 'ext:feedback:%' ORDER BY posted_at");
    CHECK_EQ(rows.size(), std::size_t{2});
    for (const auto& r : rows) CHECK_EQ(r.str("speaker_id"), std::string("ext:feedback:anon"));
    bool has_kami = false;
    for (const auto& r : rows) has_kami = has_kami || str::contains(r.str("raw_content"), "神ゲー");
    CHECK(has_kami);
    for (const auto& c : core->raw().table_columns("utterances")) {
        const std::string lc = str::to_lower_ascii(c);
        CHECK(!str::contains(lc, "author") && !str::contains(lc, "name"));
    }
    // 賛否シグナル → reactions
    CHECK_EQ(core->raw().get("SELECT COUNT(*) AS c FROM reactions")->i64("c"), 2);

    // 日本語タイトル + slug 未指定
    LearningFlowDeps ja;
    ja.core = core.get();
    ja.gamesDir = games_dir;
    ja.opinions = {LearningOpinion{"モンストは引っ張りが楽しい、神ゲー", 1700000000000.0}};
    ja.log = [](const std::string&) {};
    const auto jr = run_learning_flow("モンスターストライク", "モンスターストライクの面白さ", ja);
    CHECK_EQ(jr.gameSlug, std::string("モンスターストライク"));
    CHECK_EQ(jr.opinionsRecorded, 1);
    CHECK(core->raw().get("SELECT 1 FROM utterances WHERE id = ?", {"ext:feedback:learning:モンスターストライク:1700000000000:0"}).has_value());

    // 自動収集モード
    std::vector<std::pair<std::string, std::string>> calls;
    LearningFlowDeps cr;
    cr.core = core.get();
    cr.slug = "test-game";
    cr.gamesDir = games_dir;
    cr.log = [](const std::string&) {};
    LearningCrawlSpec spec;
    spec.sources = {"niconico", "youtube"};
    spec.maxItems = 50;
    spec.youtubeApiKey = "dummy-key";
    spec.specBySource["niconico"] = LearningSourceParams{std::nullopt, std::nullopt, std::string("sub")};
    cr.crawl = spec;
    cr.collectAndImportFn = [&](const CollectAndImportDeps& c) {
        calls.emplace_back(c.spec.source, c.spec.query.value_or(""));
        CHECK_EQ(c.maxItems.value_or(0), 50);
        CHECK_EQ(c.slug, std::string("test-game"));
        if (c.spec.source == "niconico") CHECK_EQ(c.spec.subreddit.value_or(""), std::string("sub"));
        return CollectAndImportResult{3, c.spec.source == "niconico" ? 3 : 2};
    };
    const auto crr = run_learning_flow("Test Game", "Test Game の面白さ", cr);
    CHECK_EQ(crr.opinionsRecorded, 0);
    CHECK_EQ(crr.crawledImported, 5);
    CHECK_EQ(json_stringify(crr.crawledBySource), std::string(R"({"niconico":3,"youtube":2})"));
    CHECK(calls.size() == 2 && calls[0].first == "niconico" && calls[1].first == "youtube");
    CHECK(calls[0].second == "Test Game");

    // 1 ソース失敗は止めない
    LearningFlowDeps pa = cr;
    pa.crawl->query = " クエリ ";
    std::vector<std::string> warns;
    pa.warn = [&](const std::string& w) { warns.push_back(w); };
    pa.collectAndImportFn = [](const CollectAndImportDeps& c) -> CollectAndImportResult {
        if (c.spec.source == "youtube") throw std::runtime_error("youtube boom");
        CHECK_EQ(c.spec.query.value_or(""), std::string("クエリ"));
        return {4, 4};
    };
    const auto pr = run_learning_flow("Test Game", "Test Game の面白さ", pa);
    CHECK_EQ(pr.crawledImported, 4);
    CHECK_EQ(json_stringify(pr.crawledBySource), std::string(R"({"niconico":4})"));
    CHECK(warns.size() == 1 && warns[0] == "自動収集失敗 (source=youtube): youtube boom");
    core->close();
}

DI_TEST_MAIN()
