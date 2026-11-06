// engine.ts / index.ts の移植の検証: tests/persona-engine/engine.test.ts と safety-caps.test.ts の観点、
// tick (start/stop・二重 start・stop 後に走らない)。
#include "check.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include "base/strings.hpp"
#include "persona_engine/index.hpp"
#include "w2i_support.hpp"

using namespace di;
using namespace di::persona_engine;

namespace {

std::shared_ptr<w2i::RecordingProvider> make_ctx() {
    auto ctx = std::make_shared<w2i::RecordingProvider>();
    ContextHypothesis h;
    h.id = "h-stale";
    h.statement = "stale hypothesis";
    h.status = "proposed";
    h.designGapId = "g-1";
    h.updatedAt = 1;
    ctx->hypotheses.push_back(h);
    ContextGap g;
    g.id = "g-1";
    g.title = "gap test";
    g.status = "open";
    g.hypothesisIds = {"h-stale"};
    g.updatedAt = 2;
    ctx->gaps.push_back(g);
    return ctx;
}

MockResponder text(std::string s) {
    return [s](const LLMInvokeArgs&) { return std::variant<std::string, LLMResult>(s); };
}

std::string last_log_detail(const sql::DatabasePtr& db) { return db->get("SELECT detail FROM rule_log ORDER BY id DESC LIMIT 1")->str("detail"); }

void clear_cooldowns(const sql::DatabasePtr& db) { db->run("UPDATE rules SET last_fired_at = NULL"); }

} // namespace

TEST_CASE("engine: fire 経路 (propose / cooldown / invalid JSON / skip / event / kill switch)") {
    auto db = w2i::temp_db("engine");
    auto ctx = make_ctx();
    auto logger = std::make_shared<w2i::RecordingLogger>();
    auto llm = std::make_shared<MockLLMClient>(std::vector<MockResponder>{
        text(R"({"action":"propose_hypothesis","statement":"絞ったオンボーディング","addresses_gap_id":"g-1","reasoning":"test"})"),
        text("not a json {{"),
        text(R"({"action":"skip","reasoning":"nothing to say"})"),
    });
    PersonaEngineOptions o;
    o.db = db;
    o.llm = llm;
    o.contextProvider = ctx;
    o.logger = logger;
    o.workspaceId = "knowledge";
    o.tickMs = 60'000;
    o.enableLlm = true;
    auto engine = create_persona_engine(o);

    engine->fire_rule("propose-on-gap", "manual");
    CHECK_EQ(ctx->proposed.size(), std::size_t(1));
    CHECK_EQ(ctx->proposed[0].statement, std::string("絞ったオンボーディング"));
    CHECK_EQ(*ctx->proposed[0].designGapId, std::string("g-1"));
    CHECK_EQ(ctx->proposed[0].byPersonaId, std::string("advocate"));
    CHECK_EQ(*llm->last_invocation()->personaId, std::string("advocate"));

    engine->fire_rule("propose-on-gap", "manual-retry");  // cooldown
    CHECK_EQ(ctx->proposed.size(), std::size_t(1));
    CHECK_EQ(llm->calls(), 1);

    engine->fire_rule("refute-cold", "manual");
    bool warned = false;
    for (const auto& c : logger->calls)
        if (c["level"] == "warn" && str::contains(c["msg"].get<std::string>(), "invalid JSON")) warned = true;
    CHECK(warned);

    engine->fire_rule("integrate-on-many", "manual");
    CHECK(ctx->posted.empty());
    CHECK_EQ(ctx->proposed.size(), std::size_t(1));

    engine->fire_event({"GapDetected", std::nullopt, std::nullopt});
    engine->fire_rule("no-such-rule", "manual");  // 見つからなければ warn だけ

    engine->set_rules_enabled(false);
    clear_cooldowns(db);
    engine->fire_rule("refute-cold", "manual");
    CHECK(str::contains(last_log_detail(db), "runtime"));
    engine->stop();
}

TEST_CASE("engine: safety caps (session / rule / reset / tick は対象外)") {
    auto db = w2i::temp_db("safety");
    auto ctx = make_ctx();
    std::vector<MockResponder> rs;
    for (int i = 0; i < 20; ++i)
        rs.push_back(text(R"({"action":"propose_hypothesis","statement":"S)" + std::to_string(i) + R"(","addresses_gap_id":"g-1","reasoning":"r"})"));
    auto llm = std::make_shared<MockLLMClient>(rs);
    PersonaEngineOptions o;
    o.db = db;
    o.llm = llm;
    o.contextProvider = ctx;
    o.logger = std::make_shared<w2i::RecordingLogger>();
    o.workspaceId = "knowledge";
    o.tickMs = 60'000;
    o.maxFiresPerSession = 2;
    o.maxFiresPerRulePerSession = 1;
    auto engine = create_persona_engine(o);

    db->run("UPDATE rules SET last_fired_at = NULL, cooldown_sec = 0");
    engine->fire_rule("propose-on-gap", "manual", std::string("session-A"));
    CHECK_EQ(ctx->proposed.size(), std::size_t(1));
    clear_cooldowns(db);
    engine->fire_rule("propose-on-gap", "manual-2", std::string("session-A"));
    CHECK_EQ(ctx->proposed.size(), std::size_t(1));
    CHECK(str::contains(last_log_detail(db), "rule cap reached"));
    clear_cooldowns(db);
    engine->fire_rule("refute-cold", "manual", std::string("session-A"));
    CHECK_EQ(ctx->proposed.size(), std::size_t(2));
    clear_cooldowns(db);
    engine->fire_rule("integrate-on-many", "manual", std::string("session-A"));
    CHECK_EQ(ctx->proposed.size(), std::size_t(2));
    CHECK(str::contains(last_log_detail(db), "session cap reached"));
    clear_cooldowns(db);
    engine->fire_rule("propose-on-gap", "manual", std::string("session-B"));
    CHECK_EQ(ctx->proposed.size(), std::size_t(3));
    engine->reset_session("session-A");
    clear_cooldowns(db);
    engine->fire_rule("propose-on-gap", "manual", std::string("session-A"));
    CHECK_EQ(ctx->proposed.size(), std::size_t(4));
    const auto before = ctx->proposed.size();
    for (int i = 0; i < 5; ++i) {
        clear_cooldowns(db);
        engine->fire_rule("propose-on-gap", "tick", std::nullopt);
    }
    CHECK(ctx->proposed.size() - before >= 5);
    // 空の sessionId も TS と同じく cap 対象外
    clear_cooldowns(db);
    engine->fire_rule("propose-on-gap", "x", std::string(""));
    CHECK_EQ(ctx->proposed.size(), before + 6);

    // restart-recovery: 同じ db で作り直しても session のカウンタが残る
    auto engine2 = create_persona_engine(o);
    clear_cooldowns(db);
    engine2->fire_rule("propose-on-gap", "manual", std::string("session-B"));
    CHECK(str::contains(last_log_detail(db), "cap reached"));
}

TEST_CASE("engine: tick は start で動き、二重 start せず、stop 後は走らない") {
    auto db = w2i::temp_db("tick");
    auto ctx = make_ctx();
    std::atomic<int> calls{0};
    std::vector<MockResponder> rs;
    auto llm = std::make_shared<MockLLMClient>(rs, R"({"action":"skip","reasoning":"tick"})");
    PersonaEngineOptions o;
    o.db = db;
    o.llm = llm;
    o.contextProvider = ctx;
    o.logger = std::make_shared<w2i::RecordingLogger>();
    o.workspaceId = "knowledge";
    o.tickMs = 20;
    // tick rule を 1 つだけ (tick_sec 1、cooldown 0)
    RuleSeed s;
    s.id = "only-tick";
    s.trigger_type = "tick";
    s.tick_sec = 1;
    s.cooldown_sec = 0;
    s.target = std::string("advocate");
    s.instructions = "tick で発火する rule (テスト用)";
    o.ruleSeeds = std::vector<RuleSeed>{s};
    auto engine = create_persona_engine(o);
    engine->start();
    engine->start();  // 二重 start しない
    CHECK(engine->running());
    for (int i = 0; i < 200 && llm->calls() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(llm->calls() >= 1);
    engine->stop();
    CHECK(!engine->running());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 走りかけの tick を待つ
    const int after_stop = llm->calls();
    clear_cooldowns(db);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    CHECK_EQ(llm->calls(), after_stop);
    engine->fire_rule("only-tick", "manual");  // stop 後は manual も走らない
    CHECK_EQ(llm->calls(), after_stop);
    // 再 start で再び動く
    clear_cooldowns(db);
    engine->start();
    for (int i = 0; i < 200 && llm->calls() == after_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(llm->calls() > after_stop);
    engine->stop();
    (void)calls;
}

TEST_CASE("engine: enableLlm=false / target 無し / persona 無し / LLM 失敗 / 例外") {
    auto db = w2i::temp_db("misc");
    auto ctx = make_ctx();
    auto llm = std::make_shared<MockLLMClient>(std::vector<MockResponder>{
        [](const LLMInvokeArgs&) { return std::variant<std::string, LLMResult>(LLMResult::failure("boom")); },
        [](const LLMInvokeArgs&) -> std::variant<std::string, LLMResult> { throw std::runtime_error("kaboom"); },
    });
    PersonaEngineOptions o;
    o.db = db;
    o.llm = llm;
    o.contextProvider = ctx;
    o.logger = std::make_shared<w2i::RecordingLogger>();
    o.workspaceId = "knowledge";
    o.tickMs = 60'000;
    auto engine = create_persona_engine(o);
    engine->fire_rule("propose-on-gap", "manual");
    CHECK_EQ(last_log_detail(db), std::string("llm error: boom"));
    clear_cooldowns(db);
    engine->fire_rule("propose-on-gap", "manual");
    CHECK_EQ(last_log_detail(db), std::string("kaboom"));
    db->run("UPDATE rules SET target = NULL WHERE id = 'refute-cold'");
    engine->fire_rule("refute-cold", "manual");
    CHECK_EQ(last_log_detail(db), std::string("rule.target persona_id not set"));
    db->run("UPDATE rules SET target = 'ghost' WHERE id = 'integrate-on-many'");
    engine->fire_rule("integrate-on-many", "manual");
    CHECK_EQ(last_log_detail(db), std::string("persona not found: ghost"));

    o.enableLlm = false;
    o.seedDefaults = false;
    auto engine2 = create_persona_engine(o);
    clear_cooldowns(db);
    engine2->fire_rule("propose-on-gap", "manual");
    CHECK_EQ(last_log_detail(db), std::string("llm disabled in deps"));
}

DI_TEST_MAIN()
