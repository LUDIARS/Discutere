#include "check.hpp"

#include <atomic>
#include <chrono>
#include <set>
#include <thread>

#include "flow/debater_templates.hpp"
#include "flow/dialectic/argument_pattern.hpp"
#include "flow/dialectic/histrio_context.hpp"
#include "infra/http_server.hpp"
#include "integrations/histrio_client.hpp"

using namespace di;
using namespace di::flow;
using namespace di::flow::dialectic;
using di::integrations::HistrioClient;
using namespace std::chrono_literals;

namespace {

Json profile_for(std::string id) {
    return {{"id", std::move(id)}, {"namespace", "di"}, {"script_pattern_id", "until"},
            {"script_pattern_version", "hi-v1"}, {"egogram", {{"CP", 1}, {"NP", 72}, {"A", 3}, {"FC", 4}, {"AC", 5}}}};
}

Json patterns() {
    return Json::array({{{"id", "until"}, {"namespace", "shared"}, {"version", "hi-v1"}, {"label", "Histrio 脚本"},
                         {"healthy", "前提を確認する"}, {"stress", "不足条件を問い直す"}}});
}

struct Stub {
    std::shared_ptr<http::Router> router = std::make_shared<http::Router>();
    http::Server server{http::ServerOptions{.port = 0}, router};

    std::string start() {
        server.start();
        return "http://127.0.0.1:" + std::to_string(server.bound_port());
    }
};

FlowPersona pool_persona(std::string id) {
    FlowPersona persona;
    persona.id = std::move(id);
    persona.poolPersonaId = persona.id;
    persona.role = "debater";
    persona.stance = "con";
    persona.scriptPattern = "always";
    persona.reasoningStyle = "deduction";
    return persona;
}

FlowPersona proxy_persona(std::string source, std::optional<std::string> histrio_id) {
    FlowPersona persona;
    persona.id = "internal:" + source;
    persona.role = "debater";
    persona.proxy.emplace();
    persona.proxy->speakerKey = "ext:" + source + ":d2e0afe3337ca564";
    persona.proxy->histrioPersonaId = std::move(histrio_id);
    return persona;
}

} // namespace

TEST_CASE("Histrio parses namespace-scoped responses and classifies missing and invalid profiles") {
    Stub stub;
    stub.router->get("/v1/personas/:id/profile", [](http::Request& request) {
        if (request.query("namespace") != "di") return http::Response::text("bad namespace", 400);
        const auto id = request.param("id");
        if (id == "missing") return http::Response::text("unknown", 404);
        if (id == "broken") return http::Response::text("{");
        if (id == "oversized") return http::Response::text(std::string(513u << 10, 'x'));
        return http::Response::json(profile_for(id == "wrong-id" ? "other" : id));
    });
    stub.router->get("/v1/script-patterns", [](http::Request& request) {
        if (request.query("namespace") != "di") return http::Response::text("bad namespace", 400);
        return http::Response::json(patterns());
    });
    HistrioClient client(stub.start() + "/");
    const auto found = client.profile("available");
    CHECK(found.value);
    CHECK_EQ(found.value->patternId, std::string("until"));
    CHECK_EQ(found.value->patternVersion, std::string("hi-v1"));
    CHECK_EQ(found.value->tone.value_or(""), std::string("相手を気遣って"));
    const auto catalog = client.script_patterns();
    CHECK(catalog.value);
    CHECK_EQ(catalog.value->at(0).healthy, std::string("前提を確認する"));
    for (const auto* id : {"missing", "broken", "wrong-id", "oversized"}) {
        const auto result = client.profile(id);
        CHECK(!result.value);
        CHECK(!result.error.empty());
    }
    CHECK(client.profile("missing").error.find("404") != std::string::npos);
    CHECK(client.profile("broken").error.find("JSON") != std::string::npos);
}

TEST_CASE("Histrio stops a slow response at the request deadline") {
    Stub stub;
    stub.router->get("/v1/personas/:id/profile", [](http::Request&) {
        std::this_thread::sleep_for(3s);
        return http::Response::json(profile_for("slow"));
    });
    HistrioClient client(stub.start());
    const auto start = std::chrono::steady_clock::now();
    const auto result = client.profile("slow");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(!result.value);
    CHECK(result.error.find("1.5") != std::string::npos);
    CHECK(elapsed >= 1400ms && elapsed < 2400ms);
}

TEST_CASE("Histrio uses template and Steam IDs and retains assigned patterns on 404 or malformed profiles") {
    std::atomic<int> profiles{0};
    std::atomic<int> catalogs{0};
    Stub stub;
    stub.router->get("/v1/personas/:id/profile", [&](http::Request& request) {
        ++profiles;
        const auto id = request.param("id");
        if (request.query("namespace") != "di" ||
            (id != "tpl-available" && id != "tpl-missing" && id != "tpl-broken" && id != "steam-reviewer"))
            return http::Response::text("unexpected ID or namespace", 400);
        if (id == "tpl-missing") return http::Response::text("unknown", 404);
        if (id == "tpl-broken") return http::Response::json(profile_for("wrong"));
        return http::Response::json(profile_for(id));
    });
    stub.router->get("/v1/script-patterns", [&](http::Request&) {
        ++catalogs;
        return http::Response::json(patterns());
    });
    const HistrioClient client(stub.start());
    auto cast = std::vector{pool_persona("pool-a"), pool_persona("pool-b"), pool_persona("pool-c"),
                            proxy_persona("steam", "steam-reviewer"), proxy_persona("steam", {}),
                            proxy_persona("youtube", "steam-reviewer"), pool_persona("pool-facilitator"),
                            pool_persona("generated"), pool_persona("pool-same-template")};
    cast[0].debaterTemplateId = "tpl-available";
    cast[1].debaterTemplateId = "tpl-missing";
    cast[2].debaterTemplateId = "tpl-broken";
    cast[6].role = "facilitator";
    cast[7].poolPersonaId.reset();
    cast[8].debaterTemplateId = cast[0].debaterTemplateId;
    const auto& local = argument_pattern_catalog();
    std::vector<std::string> warnings;
    const HistrioClient disabled("");
    CHECK(!disabled.enabled());
    CHECK(disabled.profile("available").error.empty());
    CHECK(!disabled.script_patterns().value);
    apply_histrio_context(cast, disabled, local, {});
    CHECK_EQ(cast[0].scriptPattern.value_or(""), std::string("always"));
    CHECK_EQ(profiles.load(), 0);
    CHECK_EQ(catalogs.load(), 0);
    apply_histrio_context(cast, client, local, [&](const std::string& message) { warnings.push_back(message); });
    CHECK_EQ(profiles.load(), 4);
    CHECK_EQ(catalogs.load(), 1);
    CHECK_EQ(warnings.size(), 2u);
    CHECK(warnings[0].find("tpl-broken") != std::string::npos);
    CHECK(warnings[1].find("tpl-missing") != std::string::npos);
    CHECK_EQ(cast[0].stance, std::string("con"));
    CHECK(cast[0].lifeScript);
    CHECK_EQ(cast[0].lifeScript->source, std::string("histrio"));
    CHECK_EQ(cast[0].lifeScript->version, std::string("hi-v1"));
    const auto opening = argument_pattern_lines(cast[0], ArgumentPhase::position, false, local);
    CHECK(opening[0].find("Histrio 脚本") != std::string::npos);
    CHECK(opening[0].find("前提を確認する") != std::string::npos);
    CHECK(opening[1].find("口調") != std::string::npos);
    const auto pressed = argument_pattern_lines(cast[0], ArgumentPhase::exchange, true, local);
    CHECK(pressed[0].find("不足条件を問い直す") != std::string::npos);
    CHECK_EQ(cast[1].scriptPattern.value_or(""), std::string("always"));
    CHECK(!cast[1].lifeScript);
    CHECK_EQ(argument_pattern_lines(cast[1], ArgumentPhase::position, false, local).size(), 2u);
    CHECK_EQ(cast[2].scriptPattern.value_or(""), std::string("always"));
    CHECK(cast[3].lifeScript);
    for (std::size_t i : {4u, 5u, 6u, 7u}) CHECK(!cast[i].lifeScript);
    CHECK(cast[8].lifeScript);
}

TEST_CASE("Histrio uses the Di copy if only the catalog fails") {
    Stub stub;
    stub.router->get("/v1/personas/:id/profile", [](http::Request& request) {
        return http::Response::json(profile_for(request.param("id")));
    });
    stub.router->get("/v1/script-patterns", [](http::Request&) { return http::Response::text("{"); });
    const HistrioClient client(stub.start());
    auto cast = std::vector{pool_persona("available")};
    cast[0].debaterTemplateId = "tpl-available";
    const auto& local = argument_pattern_catalog();
    apply_histrio_context(cast, client, local, {});
    CHECK(cast[0].lifeScript);
    CHECK_EQ(cast[0].lifeScript->id, std::string("until"));
    CHECK_EQ(cast[0].lifeScript->source, std::string("di"));
    CHECK_EQ(cast[0].lifeScript->version, local.version);
}

TEST_CASE("Debater template assignment is deterministic and exhausts the catalog before repeating") {
    std::vector<FlowPersona> cast;
    for (int i = 0; i < 18; ++i) cast.push_back(pool_persona("pool-" + std::to_string(i)));
    cast[0].role = "facilitator";
    cast[1].poolPersonaId.reset();
    cast[2] = proxy_persona("steam", "steam-reviewer");
    auto second = cast;
    auto other_session = cast;
    assign_debater_templates(cast, "session-a");
    assign_debater_templates(second, "session-a");
    assign_debater_templates(other_session, "session-b");
    std::set<std::string> first_cycle;
    bool differs = false;
    for (std::size_t i = 0; i < cast.size(); ++i) {
        CHECK(cast[i].debaterTemplateId == second[i].debaterTemplateId);
        if (i < 3) {
            CHECK(!cast[i].debaterTemplateId);
            continue;
        }
        CHECK(cast[i].debaterTemplateId);
        CHECK(cast[i].debaterTemplateId->starts_with("tpl-"));
        if (i < 15) CHECK(first_cycle.insert(*cast[i].debaterTemplateId).second);
        differs = differs || cast[i].debaterTemplateId != other_session[i].debaterTemplateId;
    }
    CHECK(differs);
}

int main() { return di::test::run_all(); }
