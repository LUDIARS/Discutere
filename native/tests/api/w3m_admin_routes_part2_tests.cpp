// w3m_admin_routes のテスト (2/3)。準備は w3m_admin_routes_support.hpp。
#include "w3m_admin_routes_support.hpp"

using namespace w3m_admin_routes_support;

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

DI_TEST_MAIN()
