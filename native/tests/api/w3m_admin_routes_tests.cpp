// w3-M: api/admin-routes.ts・consensus-persona-routes.ts・gap-creation-routes.ts の移植の検証 (TS テスト無し)。
// 認可 (401/403)・late-binding の 503・各ルートの効果と応答の形・本文の境界 (上限ちょうど/+1・壊れた JSON・巨大)。
#include "check.hpp"

#include <filesystem>

#include "api/admin_routes.hpp"
#include "api/consensus_persona_routes.hpp"
#include "api/gap_creation_routes.hpp"
#include "base/env.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/test_util.hpp"
#include "discord_hook/reactions.hpp"
#include "middleware/auth.hpp"
#include "persona_engine/facilitator/facilitator.hpp"
#include "persona_engine/index.hpp"
#include "persona_engine/llm/mock.hpp"
#include "persona_engine/scoring/consensus_scorer.hpp"
#include "w1f_support.hpp"

using namespace di;
using di::test::w1f::body_json;
using di::test::w1f::make_request;

namespace {

const std::string WS = "ws-test";

class NoopProvider : public persona_engine::DiscussionContextProvider {
public:
    explicit NoopProvider(core::Core* core = nullptr) : core_(core) {}
    std::vector<persona_engine::ContextHypothesis> list_active_hypotheses(const std::string&, int) override { return {}; }
    std::vector<persona_engine::ContextGap> list_recent_gaps(const std::string&, int) override { return {}; }
    std::vector<persona_engine::ContextUtterance> list_recent_utterances(const std::string&, const std::string&, int) override {
        return {};
    }
    std::string propose_hypothesis(const persona_engine::ProposeHypothesisInput&) override { return ""; }
    std::string post_utterance(const persona_engine::PostUtteranceInput& in) override {
        if (!core_) return "";
        core::repositories::UtteranceInput u;
        u.workspaceId = in.workspaceId;
        u.sessionId = in.sessionId;
        u.speakerId = "persona:" + in.byPersonaId;
        u.rawContent = in.text;
        u.postedAt = static_cast<double>(time::now_ms());
        return core_->repos.utterance.create(u);
    }

private:
    core::Core* core_;
};

/** active KG / persona-engine DB / workspace を一時ディレクトリへ向けた環境。 */
struct Env {
    test::TempDir dir;
    std::string kg;
    std::string pe;
    Env() {
        kg = dir.file("kg.kuzu");
        pe = dir.file("pe.db");
        env::set_for_test("DISCUTERE_CONFIG", std::string_view(dir.file("no-config.json")));
        env::set_for_test("DISCATIER_KUZU_PATH", std::string_view(kg));
        env::set_for_test("DISCUTERE_PERSONA_ENGINE_DB", std::string_view(pe));
        env::set_for_test("DISCATIER_WORKSPACE", std::string_view(WS));
        config::reset_config_for_test();
        api::set_persona_engine(nullptr);
        api::set_facilitator(nullptr);
        api::set_gateway_sweeper(nullptr);
    }
    ~Env() {
        api::set_persona_engine(nullptr);
        api::set_facilitator(nullptr);
        api::set_gateway_sweeper(nullptr);
        config::reset_config_for_test();
    }
    std::unique_ptr<core::Core> core() { return core::create_core(kg, dir.file("events.jsonl")); }
};

http::Router app() {
    http::Router r;
    r.use(middleware::user_context());
    r.mount("/api", api::admin_routes());
    r.mount("/api", api::consensus_persona_routes());
    r.mount("/api", api::gap_creation_routes());
    return r;
}

http::Response call(const http::Router& r, const std::string& method, const std::string& target, std::string body = "",
                    const Json& headers = Json::object(), const std::string& remote = "127.0.0.1") {
    auto req = make_request(method, target, headers, remote, std::move(body));
    return r.handle(req);
}

struct Gap {
    std::string gapId;
    std::string sessionId;
};
Gap make_gap(core::Core& c, const std::string& title, const std::string& scene, const std::optional<std::string>& status = std::nullopt) {
    core::repositories::DesignGapInput g;
    g.workspaceId = WS;
    g.title = title;
    g.status = status;
    Gap out;
    out.gapId = c.repos.designGap.create(g);
    core::repositories::SessionInput s;
    s.workspaceId = WS;
    s.title = "discussion-of-gap:" + out.gapId;
    s.startedAt = 1000;
    s.scene = scene;
    out.sessionId = c.repos.session.create(s);
    return out;
}

std::shared_ptr<persona_engine::PersonaEngineHandle> make_engine(const std::string& pe_path) {
    persona_engine::PersonaEngineOptions o;
    o.db = sql::Database::open(pe_path);
    o.llm = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{});
    o.contextProvider = std::make_shared<NoopProvider>();
    o.workspaceId = WS;
    o.enableLlm = false;
    return persona_engine::create_persona_engine(o);
}

const std::vector<std::pair<std::string, std::string>> kAdminRoutes = {
    {"POST", "/api/admin/rules/enabled"},       {"POST", "/api/admin/session/reset"},    {"POST", "/api/admin/gaps/g1/dismiss"},
    {"POST", "/api/admin/gaps/dismiss-open"},   {"POST", "/api/admin/sessions/s1/close"}, {"POST", "/api/admin/sessions/s1/converge"},
    {"GET", "/api/admin/status"},               {"POST", "/api/admin/seed-sweep"},       {"GET", "/api/admin/learning/topics"},
    {"GET", "/api/admin/metrics/personas"},     {"GET", "/api/admin/metrics/rules"},     {"GET", "/api/admin/rules"},
    {"GET", "/api/admin/rules/r1/logs"},        {"GET", "/api/admin/consensus/g1/personas"}, {"POST", "/api/admin/gaps"},
};

} // namespace

TEST_CASE("認可: 非 loopback の匿名・role≠admin は 403、userId 無し (middleware 無し) は 401") {
    Env env;
    const auto r = app();
    for (const auto& [m, path] : kAdminRoutes) {
        auto res = call(r, m, path, "{}", Json{{"content-type", "application/json"}}, "203.0.113.5");
        CHECK_EQ(res.status, 403);
        CHECK_EQ(body_json(res)["error"], Json("admin role required"));
        res = call(r, m, path, "{}", Json{{"X-User-Id", "u1"}, {"X-User-Role", "general"}, {"content-type", "application/json"}});
        CHECK_EQ(res.status, 403);
        // consensus routes は guard を通った後にだけ Cache-Control を付ける
        CHECK(!res.headers.has("Cache-Control"));
    }
    http::Router bare;
    bare.mount("/api", api::admin_routes());
    bare.mount("/api", api::consensus_persona_routes());
    bare.mount("/api", api::gap_creation_routes());
    for (const auto& [m, path] : kAdminRoutes) {
        const auto res = call(bare, m, path, "{}", Json{{"content-type", "application/json"}});
        CHECK_EQ(res.status, 401);
        CHECK_EQ(body_json(res)["error"], Json("Authentication required"));
    }
}

TEST_CASE("late-binding: engine / facilitator / sweeper 未注入は 503、status は未接続を返す") {
    Env env;
    const auto r = app();
    for (const auto& path : {"/api/admin/rules/enabled", "/api/admin/session/reset"}) {
        const auto res = call(r, "POST", path, "{}");
        CHECK_EQ(res.status, 503);
        CHECK_EQ(body_json(res)["error"], Json("persona-engine not initialized"));
    }
    for (const auto& path : {"/api/admin/metrics/personas", "/api/admin/metrics/rules", "/api/admin/rules", "/api/admin/rules/x/logs"})
        CHECK_EQ(call(r, "GET", path).status, 503);
    auto res = call(r, "POST", "/api/admin/sessions/s/converge");
    CHECK_EQ(res.status, 503);
    CHECK_EQ(body_json(res)["error"], Json("facilitator not initialized"));
    res = call(r, "POST", "/api/admin/seed-sweep", "{}");
    CHECK_EQ(res.status, 503);
    CHECK_EQ(body_json(res)["error"], Json("gateway not initialized"));
    res = call(r, "GET", "/api/admin/status");
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.body, std::string(R"({"persona_engine_attached":false,"rules_runtime_enabled":true,"persona_count":0,"rule_count":0,"recent_rule_logs":[]})"));
}

TEST_CASE("engine: rules/enabled・session/reset・status・metrics・rules・logs") {
    Env env;
    auto engine = make_engine(env.pe);
    api::set_persona_engine(engine);
    CHECK(api::get_persona_engine() == engine);
    const auto r = app();

    auto res = call(r, "POST", "/api/admin/rules/enabled", R"({"enabled":false})");
    CHECK_EQ(res.body, std::string(R"({"ok":true,"enabled":false})"));
    res = call(r, "GET", "/api/admin/status");
    auto st = body_json(res);
    CHECK_EQ(st["persona_engine_attached"], Json(true));
    CHECK_EQ(st["rules_runtime_enabled"], Json(false));
    CHECK(st["persona_count"].get<int>() > 0);
    CHECK(st["rule_count"].get<int>() > 0);
    // 壊れた JSON は {enabled:true} とみなす (TS の .catch)
    res = call(r, "POST", "/api/admin/rules/enabled", "{broken");
    CHECK_EQ(res.body, std::string(R"({"ok":true,"enabled":true})"));
    // 真偽は JS の truthy
    CHECK_EQ(call(r, "POST", "/api/admin/rules/enabled", R"({"enabled":0})").body, std::string(R"({"ok":true,"enabled":false})"));
    CHECK_EQ(call(r, "POST", "/api/admin/rules/enabled", R"({"enabled":"x"})").body, std::string(R"({"ok":true,"enabled":true})"));
    CHECK_EQ(call(r, "POST", "/api/admin/rules/enabled", R"([1])").body, std::string(R"({"ok":true,"enabled":false})"));

    res = call(r, "POST", "/api/admin/session/reset", R"({"sessionId":""})");
    CHECK_EQ(res.status, 400);
    CHECK_EQ(body_json(res)["error"], Json("sessionId required"));
    CHECK_EQ(call(r, "POST", "/api/admin/session/reset", "not json").status, 400);
    res = call(r, "POST", "/api/admin/session/reset", R"({"sessionId":"s-1"})");
    CHECK_EQ(res.body, std::string(R"({"ok":true,"sessionId":"s-1"})"));

    res = call(r, "GET", "/api/admin/metrics/personas");
    CHECK_EQ(res.status, 200);
    CHECK(body_json(res)["metrics"].is_array());
    CHECK(body_json(call(r, "GET", "/api/admin/metrics/rules"))["metrics"].is_array());

    res = call(r, "GET", "/api/admin/rules");
    const auto rules = body_json(res)["rules"];
    CHECK(!rules.empty());
    const auto& first = rules[0];
    std::vector<std::string> keys;
    for (auto it = first.begin(); it != first.end(); ++it) keys.push_back(it.key());
    CHECK_EQ(str::join(keys, ","), std::string("id,description,trigger_type,tick_sec,event_kind,target,cooldown_sec,last_fired_at,enabled,added_at,"
                                               "added_by,removed_at,removed_by,removed_reason,is_seed,is_ai_added,is_ai_removed"));
    CHECK_EQ(first["is_seed"], Json(true));
    const auto id = first["id"].get<std::string>();
    engine->rules().log({id, "fire", "engine", std::string("d1")});
    engine->rules().log({id, "skip", "engine", std::string("d2")});
    engine->rules().log({std::string("other"), "fire", "engine", std::nullopt});
    res = call(r, "GET", "/api/admin/rules/" + id + "/logs?limit=1");
    auto logs = body_json(res);
    CHECK_EQ(logs["rule_id"], Json(id));
    CHECK_EQ(logs["logs"].size(), std::size_t(1));
    CHECK_EQ(body_json(call(r, "GET", "/api/admin/rules/" + id + "/logs"))["logs"].size(), std::size_t(2));
    // 境界: limit=0 → 1、NaN → 0 件、巨大 → 200 にクランプ、小数は切り捨て
    CHECK_EQ(body_json(call(r, "GET", "/api/admin/rules/" + id + "/logs?limit=0"))["logs"].size(), std::size_t(1));
    CHECK_EQ(body_json(call(r, "GET", "/api/admin/rules/" + id + "/logs?limit=abc"))["logs"].size(), std::size_t(0));
    CHECK_EQ(body_json(call(r, "GET", "/api/admin/rules/" + id + "/logs?limit=1e9"))["logs"].size(), std::size_t(2));
    CHECK_EQ(body_json(call(r, "GET", "/api/admin/rules/" + id + "/logs?limit=1.9"))["logs"].size(), std::size_t(1));
}

TEST_CASE("gaps: dismiss / dismiss-open / sessions close (scene 単位) / learning topics") {
    Env env;
    auto c = env.core();
    const auto a = make_gap(*c, "A", "discord:1/2");
    const auto b = make_gap(*c, "B", "discord:1/3", std::string("closed"));
    const auto d = make_gap(*c, "D", "gap:x");
    // A と同じ scene を共有する intake session
    core::repositories::SessionInput intake;
    intake.workspaceId = WS;
    intake.title = "intake";
    intake.startedAt = 1;
    intake.scene = "discord:1/2";
    const auto intake_id = c->repos.session.create(intake);
    c.reset();
    const auto r = app();

    auto res = call(r, "POST", "/api/admin/sessions/" + a.sessionId + "/close");
    CHECK_EQ(res.body, std::string(R"({"ok":true,"endedSessions":2,"closedGaps":1})"));
    CHECK_EQ(call(r, "POST", "/api/admin/sessions/" + a.sessionId + "/close").body, std::string(R"({"ok":true,"endedSessions":0,"closedGaps":0})"));
    res = call(r, "POST", "/api/admin/sessions/nope/close");
    CHECK_EQ(res.status, 404);
    CHECK_EQ(body_json(res)["error"], Json("session not found"));
    c = env.core();
    CHECK(!c->raw().get("SELECT ended_at FROM sessions WHERE id = ?", {intake_id})->is_null("ended_at"));
    c.reset();

    CHECK_EQ(call(r, "POST", "/api/admin/gaps/" + b.gapId + "/dismiss").body, std::string(R"({"ok":true,"dismissed":0})"));  // closed は却下しない
    CHECK_EQ(call(r, "POST", "/api/admin/gaps/" + d.gapId + "/dismiss").body, std::string(R"({"ok":true,"dismissed":1})"));
    const auto e = [&] {
        auto cc = env.core();
        return make_gap(*cc, "E", "gap:e");
    }();
    CHECK_EQ(call(r, "POST", "/api/admin/gaps/dismiss-open").body, std::string(R"({"ok":true,"dismissed":1})"));
    c = env.core();
    CHECK_EQ(c->raw().get("SELECT status FROM design_gaps WHERE id = ?", {e.gapId})->str("status"), std::string("dismissed"));
    c.reset();

    res = call(r, "GET", "/api/admin/learning/topics?limit=abc&opinionsPerTopic=2");
    CHECK_EQ(res.status, 200);
    CHECK(body_json(res).is_object());
}

TEST_CASE("sessions converge: facilitator.converge_now を scene 共有の議論ごとに呼ぶ") {
    Env env;
    auto c = std::shared_ptr<core::Core>(env.core().release());
    const auto a = make_gap(*c, "収束 A", "discord:9/9");
    const auto b = make_gap(*c, "収束 B", "discord:9/9");
    const auto other = make_gap(*c, "別", "discord:9/8");
    auto pe = sql::Database::open(env.pe);
    persona_engine::db::apply_persona_engine_migrations(*pe);
    persona_engine::facilitator::FacilitatorDeps deps;
    deps.core = c;
    int n = 0;
    deps.llm = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [&](const persona_engine::LLMInvokeArgs&) -> std::variant<std::string, persona_engine::LLMResult> {
            return ++n == 1 ? std::variant<std::string, persona_engine::LLMResult>(std::string(R"({"summary":"まとめ"})"))
                            : std::variant<std::string, persona_engine::LLMResult>(persona_engine::LLMResult::failure("down"));
        },
        [&](const persona_engine::LLMInvokeArgs&) -> std::variant<std::string, persona_engine::LLMResult> {
            return persona_engine::LLMResult::failure("down");
        }});
    deps.contextProvider = std::make_shared<NoopProvider>(c.get());
    deps.personas = std::make_shared<persona_engine::db::PersonasRepo>(pe);
    deps.workspaceId = WS;
    api::set_facilitator(persona_engine::facilitator::create_facilitator(deps));
    const auto r = app();
    const auto res = call(r, "POST", "/api/admin/sessions/" + a.sessionId + "/converge");
    const auto j = body_json(res);
    CHECK_EQ(j["ok"], Json(true));
    CHECK_EQ(j["converged"], Json(1));
    CHECK_EQ(j["results"].size(), std::size_t(2));
    CHECK_EQ(json_stringify(j["results"][0]), json_stringify(Json{{"sessionId", a.sessionId}, {"gapId", a.gapId}, {"ok", true}}));
    CHECK_EQ(j["results"][1]["ok"], Json(false));
    CHECK(j["results"][1].contains("error"));
    CHECK_EQ(c->raw().get("SELECT status FROM design_gaps WHERE id = ?", {other.gapId})->is_null("status"), true);
    (void)b;
    CHECK_EQ(call(r, "POST", "/api/admin/sessions/zzz/converge").status, 404);
}

TEST_CASE("seed-sweep: limit の解釈 (truthy・>0・最大 100)・失敗は 500") {
    Env env;
    std::vector<std::optional<double>> seen;
    api::set_gateway_sweeper([&](std::optional<double> limit) {
        seen.push_back(limit);
        if (limit && *limit == 13) throw std::runtime_error("sweep failed");
        return api::SweepResult{5, 2};
    });
    const auto r = app();
    CHECK_EQ(call(r, "POST", "/api/admin/seed-sweep", R"({"limit":10})").body, std::string(R"({"ok":true,"scanned":5,"seeded":2})"));
    call(r, "POST", "/api/admin/seed-sweep", R"({"limit":500})");
    call(r, "POST", "/api/admin/seed-sweep", R"({"limit":0})");
    call(r, "POST", "/api/admin/seed-sweep", R"({"limit":-3})");
    call(r, "POST", "/api/admin/seed-sweep", R"({"limit":"7"})");
    call(r, "POST", "/api/admin/seed-sweep", "broken");
    CHECK_EQ(seen.size(), std::size_t(6));
    CHECK_EQ(seen[0].value_or(-1), 10.0);
    CHECK_EQ(seen[1].value_or(-1), 100.0);
    CHECK(!seen[2].has_value());
    CHECK(!seen[3].has_value());
    CHECK_EQ(seen[4].value_or(-1), 7.0);
    CHECK(!seen[5].has_value());
    const auto res = call(r, "POST", "/api/admin/seed-sweep", R"({"limit":13})");
    CHECK_EQ(res.status, 500);
    CHECK_EQ(body_json(res)["error"], Json("sweep failed"));
}

TEST_CASE("本文の上限: 既定 1 MiB ちょうどは受け、+1 はルータが拒否する口 (max_body_for)・巨大でも壊れた JSON 扱い") {
    Env env;
    const auto r = app();
    CHECK_EQ(r.max_body_for("POST", "/api/admin/rules/enabled"), http::kDefaultMaxBody);
    CHECK_EQ(r.max_body_for("POST", "/api/admin/gaps"), http::kDefaultMaxBody);
    auto engine = make_engine(env.pe);
    api::set_persona_engine(engine);
    // ちょうど上限の JSON 本文
    std::string exact = R"({"enabled":false,"pad":")";
    exact += std::string(http::kDefaultMaxBody - exact.size() - 2, 'a') + "\"}";
    CHECK_EQ(exact.size(), http::kDefaultMaxBody);
    CHECK_EQ(call(r, "POST", "/api/admin/rules/enabled", exact).body, std::string(R"({"ok":true,"enabled":false})"));
    // 深い入れ子 (parse_json の上限) は壊れた JSON 扱い
    CHECK_EQ(call(r, "POST", "/api/admin/rules/enabled", str::repeat("[", 5000) + str::repeat("]", 5000)).body,
             std::string(R"({"ok":true,"enabled":true})"));
}

TEST_CASE("POST /admin/gaps: 415・400 (各種)・201・gameId の存在確認・DB 失敗は 503 で詳細を出さない") {
    Env env;
    const auto r = app();
    const Json json_ct{{"content-type", "Application/JSON; charset=utf-8"}};
    auto res = call(r, "POST", "/api/admin/gaps", R"({"title":"t","description":"d"})", Json{{"content-type", "text/plain"}});
    CHECK_EQ(res.status, 415);
    CHECK_EQ(call(r, "POST", "/api/admin/gaps", R"({"title":"t","description":"d"})").status, 415);  // ヘッダ無し
    for (const std::string& body : {std::string("broken"), std::string("[]"), std::string("null"), std::string(R"({"title":" ","description":"d"})"),
                                   std::string(R"({"title":"t"})"), std::string(R"({"title":"t","description":"d","gameId":""})"),
                                   std::string(R"({"title":"t","description":"d","gameId":null})"), std::string(R"({"title":1,"description":"d"})")}) {
        res = call(r, "POST", "/api/admin/gaps", body, json_ct);
        CHECK_EQ(res.status, 400);
        CHECK_EQ(body_json(res)["error"], Json("title and description are required; gameId must be non-empty when supplied"));
    }
    res = call(r, "POST", "/api/admin/gaps", R"({"title":"  議題  ","description":" 説明 "})", json_ct);
    CHECK_EQ(res.status, 201);
    const auto j = body_json(res);
    std::vector<std::string> keys;
    for (auto it = j.begin(); it != j.end(); ++it) keys.push_back(it.key());
    CHECK_EQ(str::join(keys, ","), std::string("gapId,sessionId"));
    auto c = env.core();
    const auto gap = c->raw().get("SELECT * FROM design_gaps WHERE id = ?", {j["gapId"].get<std::string>()});
    CHECK_EQ(gap->str("title"), std::string("議題"));
    CHECK_EQ(gap->str("evidence_json"), std::string(R"({"source":"api:admin-gap"})"));
    res = call(r, "POST", "/api/admin/gaps", R"({"title":"t","description":"d","gameId":"nope"})", json_ct);
    CHECK_EQ(res.status, 400);
    CHECK_EQ(body_json(res)["error"], Json("gameId must identify a game in the current workspace"));
    core::repositories::GameInput g;
    g.workspaceId = WS;
    g.title = "架空ゲーム";
    const auto game_id = c->repos.game.create(g);
    c.reset();
    res = call(r, "POST", "/api/admin/gaps", R"({"title":"t","description":"d","gameId":")" + game_id + "\"}", json_ct);
    CHECK_EQ(res.status, 201);

    // persona-engine DB を開けない (ディレクトリを指す) → 503、パスを出さない
    env::set_for_test("DISCUTERE_PERSONA_ENGINE_DB", std::string_view(env.dir.path.string()));
    config::reset_config_for_test();
    res = call(r, "POST", "/api/admin/gaps", R"({"title":"t2","description":"d"})", json_ct);
    CHECK_EQ(res.status, 503);
    CHECK_EQ(res.body, std::string(R"({"error":"gap creation unavailable"})"));
}

TEST_CASE("GET /admin/consensus/:gapId/personas: session 無しは空・集計・persona DB 不在は 503 (Cache-Control 付き)") {
    Env env;
    const auto r = app();
    auto res = call(r, "GET", "/api/admin/consensus/none/personas");
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.body, std::string(R"({"gapId":"none","personas":[]})"));
    CHECK_EQ(res.headers.get("Cache-Control").value_or(""), std::string("private, no-store"));

    auto c = env.core();
    const auto gap = make_gap(*c, "合意", "gap:c");
    // consensus_scores / opinion_scores の表を用意
    {
        auto fake_llm = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{});
        persona_engine::scoring::ConsensusScorerDeps sd;
        sd.core = std::shared_ptr<core::Core>(c.get(), [](core::Core*) {});
        sd.llm = fake_llm;
        persona_engine::scoring::ConsensusScorer scorer(sd);
    }
    auto say = [&](const std::string& sp, const std::string& text, double at) {
        core::repositories::UtteranceInput u;
        u.workspaceId = WS;
        u.sessionId = gap.sessionId;
        u.speakerId = sp;
        u.rawContent = text;
        u.postedAt = at;
        return c->repos.utterance.create(u);
    };
    const auto u1 = say("persona:p1", "意見1", 10);
    say("persona:facilitator", "進行", 11);
    say("persona:ghost", "未登録", 12);
    say("user-1", "人間", 13);
    c->raw().run("INSERT INTO consensus_scores (utterance_id, agree, score, reasoning, created_at) VALUES (?, 1, 4, 'r', 1)", {u1});
    c.reset();
    // persona DB が無い → 503
    res = call(r, "GET", "/api/admin/consensus/" + gap.gapId + "/personas");
    CHECK_EQ(res.status, 503);
    CHECK_EQ(res.body, std::string(R"({"error":"persona consensus unavailable"})"));
    CHECK_EQ(res.headers.get("Cache-Control").value_or(""), std::string("private, no-store"));
    CHECK(!std::filesystem::exists(env.pe));  // readonly / fileMustExist で作らない

    auto pe = sql::Database::open(env.pe);
    persona_engine::db::apply_persona_engine_migrations(*pe);
    persona_engine::db::PersonasRepo repo(pe);
    repo.insert_or_ignore({"p1", "論客", "論 客", "", {"冷静"}, ""});
    pe->run("UPDATE personas SET traits = 'not json' WHERE id = 'p1'");
    res = call(r, "GET", "/api/admin/consensus/" + gap.gapId + "/personas");
    CHECK_EQ(res.status, 200);
    const auto j = body_json(res);
    CHECK_EQ(j["personas"].size(), std::size_t(1));
    CHECK_EQ(j["personas"][0]["personaId"], Json("p1"));
    CHECK_EQ(j["personas"][0]["traits"], Json::array());  // 壊れた traits は []
    CHECK_EQ(j["personas"][0]["utterances"][0]["agree"], Json(true));
}

DI_TEST_MAIN()
