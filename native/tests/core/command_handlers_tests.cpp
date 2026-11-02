// w3-N: core のコマンド処理 (cross / emotion / learning / synthesis) と仮説のライフサイクル。
// tests/core/hypothesis/lifecycle.test.ts の移植 + golden (応答と積んだイベントの payload が Node と一致)。
#include "check.hpp"
#include "w3n_support.hpp"

#include "core/hypothesis/lifecycle_handler.hpp"
#include "core/hypothesis/validation_routing.hpp"
#include "core/projection/command_handlers/cross/index.hpp"
#include "core/projection/command_handlers/emotion/index.hpp"
#include "core/projection/command_handlers/learning/index.hpp"
#include "core/projection/command_handlers/synthesis/index.hpp"

using namespace di;
using namespace di::core::projection::command_handlers;

namespace {

HandlerResult dispatch(const std::string& family, const std::string& command, const HandlerContext& ctx) {
    if (family == "cross") return cross::handle_cross(ctx, command);
    if (family == "learning") {
        if (command == "define") return learning::handle_define(ctx);
        if (command == "intends") return learning::handle_intends(ctx);
        if (command == "refine") return learning::handle_refine(ctx);
        if (command == "compare") return learning::handle_compare(ctx);
        if (command == "history") return learning::handle_history(ctx);
    }
    if (family == "synthesis") {
        if (command == "refs") return synthesis::handle_refs(ctx);
        if (command == "propose") return synthesis::handle_propose(ctx);
        if (command == "addresses") return synthesis::handle_addresses(ctx);
        if (command == "jump") return synthesis::handle_jump(ctx);
        if (command == "submit") return synthesis::handle_submit(ctx);
        if (command == "validate") return synthesis::handle_validate(ctx);
        if (command == "integrate") return synthesis::handle_integrate(ctx);
        if (command == "reject") return synthesis::handle_reject(ctx);
    }
    if (family == "emotion") {
        if (command == "scene") return emotion::handle_scene(ctx);
        if (command == "done") return emotion::handle_done(ctx);
    }
    throw std::runtime_error("unknown step " + family + "/" + command);
}

} // namespace

TEST_CASE("golden: コマンドの応答と積んだイベント (payload) が Node と一致") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    const Json fx = w3n::load_fixture("w3n/command_handlers.json");
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    w3n::load_tables(*core, fx["tables"]);
    for (const auto& s : fx["steps"]) {
        const auto before = core->raw().get("SELECT COUNT(*) AS c FROM events")->i64("c");
        HandlerContext ctx;
        ctx.core = core.get();
        ctx.workspaceId = fx["workspaceId"];
        ctx.sessionId = fx["sessionId"];
        if (s.contains("personId") && s["personId"].is_string()) ctx.personId = s["personId"].get<std::string>();
        ctx.args = s["args"].get<std::vector<std::string>>();
        const std::string family = s["family"], command = s["command"];
        const auto r = dispatch(family, command, ctx);
        Json result = to_json(r);
        Json expected_result = s["result"]["value"];
        Json events = Json::array();
        for (const auto& e : core->raw().all("SELECT event_type, payload_json FROM events ORDER BY rowid LIMIT -1 OFFSET ?", {before}))
            events.push_back(Json{{"eventType", e.str("event_type")}, {"payload", parse_json_or_throw(e.str("payload_json"))}});
        Json expected_events = s["events"];
        // 乱数の id と現在時刻は比べない (両方を同じ印に置き換える)。
        auto normalize = [&](Json& evs, Json& res) {
            for (auto& e : evs) {
                if (command == "define" || command == "propose") e["payload"]["id"] = "<id>";
                if (command == "done") e["payload"]["endedAt"] = "<now>";
            }
            if ((command == "define" || command == "propose") && res.contains("message")) res["message"] = "<id>";
        };
        normalize(events, result);
        normalize(expected_events, expected_result);
        CHECK_EQ(json_stringify(result), json_stringify(expected_result));
        CHECK_EQ(json_stringify(events), json_stringify(expected_events));
    }
    core->close();
}

TEST_CASE("lifecycle.test.ts: draft → submit → validate_theory → integrate は不可・仮説が無ければ not found") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    core::repositories::HypothesisInput h;
    h.workspaceId = "w1";
    h.statement = "h1";
    h.status = "draft";
    const auto hid = core->repos.hypothesis.create(h);
    CHECK(core::hypothesis::apply_lifecycle(*core, hid, "submit", {"w1", std::string("s1")}).ok);
    CHECK(core::hypothesis::apply_lifecycle(*core, hid, "validate_theory", {"w1", std::string("s1")}).ok);
    CHECK(!core::hypothesis::apply_lifecycle(*core, hid, "integrate", {"w1", std::string("s1")}).ok);
    const auto missing = core::hypothesis::apply_lifecycle(*core, "nope", "submit", {"w1", std::nullopt});
    CHECK(!missing.ok);
    CHECK_EQ(missing.error, std::string("hypothesis not found"));
    // 壊れた refs_json は TS と同じく例外 (JSON.parse)。
    core->raw().run("UPDATE hypotheses SET refs_json = '{bad' WHERE id = ?", {hid});
    CHECK_THROWS(core::hypothesis::apply_lifecycle(*core, hid, "session_end", {"w1", std::nullopt}));
    // session で検証された仮説が無ければ 0 件。
    CHECK_EQ(core::hypothesis::finalize_validation_for_session(*core, "w1", "s-none"), 0);
    core->close();
}

DI_TEST_MAIN()
