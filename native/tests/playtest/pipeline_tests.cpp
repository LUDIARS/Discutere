#include "check.hpp"

#include <limits>

#include "api/impression_bridge_routes.hpp"
#include "core/test_util.hpp"
#include "flow/db/migrations.hpp"
#include "persona_engine/db/migrations.hpp"
#include "playtest/proposal_outbox.hpp"
#include "playtest/seed.hpp"
#include "playtest/voice_store.hpp"

using namespace di;
using namespace di::playtest;

namespace {

Json review(std::string id = "run:1", std::string game = "bestia", std::string title = "Bestia") {
    const Json point{{"aspect", "control"}, {"text", "入力が分かりやすい"},
                     {"evidence", {{"ticks", Json::array({0, 42})}, {"shots", Json::array()}}}};
    return Json{{"origin", "ai"}, {"source", "ai-autoplayer"}, {"run_id", std::move(id)}, {"game_id", std::move(game)},
                {"game_title", std::move(title)}, {"summary", "試遊のまとめ"}, {"good", Json::array({point})}, {"bad", Json::array({point})},
                {"confusions", Json::array()}, {"reflection", Json::array({"振り返り"})}, {"played_at", "2026-11-21T00:00:00.000Z"}};
}

struct Voices {
    sql::DatabasePtr db = sql::Database::open(":memory:");
    Voices() { flow::db::apply_flow_migrations(*db); }
    bool save(const std::string& id, const std::string& game = "bestia", const std::string& title = "Bestia") {
        return save_ai_review(*db, parse_ai_review(review(id, game, title), id).value());
    }
};

} // namespace

TEST_CASE("Voluptas AI review wire contract and strict nested validation") {
    const auto good = review();
    CHECK(parse_ai_review(good, "run:1").has_value());
    CHECK(!parse_ai_review(good, "other"));
    for (const auto& [key, value] : std::vector<std::pair<std::string, Json>>{
             {"origin", "human"}, {"source", "unknown"}, {"persona", "unexpected"}, {"good", Json::array()},
             {"played_at", "2026-02-30T00:00:00.000Z"}, {"game_id", 42}}) {
        auto bad = good;
        bad[key] = value;
        CHECK(!parse_ai_review(bad, "run:1"));
    }
    auto bad = good;
    bad["good"][0]["evidence"]["user_id"] = "x";
    CHECK(!parse_ai_review(bad, "run:1"));
    bad = good;
    bad["bad"][0]["evidence"]["ticks"][0] = std::numeric_limits<std::uint64_t>::max();
    CHECK(!parse_ai_review(bad, "run:1"));
}

TEST_CASE("AI review route authenticates, returns an idempotent receipt, and isolates paper evidence") {
    Voices voices;
    bool fail = false;
    const std::string token(40, 't');
    api::ImpressionBridgeRoutesOptions options;
    options.token = [token] { return token; };
    options.saveAiReview = [&](const AiReview& value) {
        if (fail) throw std::runtime_error("store failed");
        return save_ai_review(*voices.db, value);
    };
    http::Router api;
    api.mount("/api", api::create_impression_bridge_routes(options));
    auto post = [&](const Json& value, const std::string& key, bool authorized = true) {
        http::Request req;
        req.method = "POST";
        req.target = req.path = "/api/persona-bridge/ai-playtest-reviews";
        req.headers.set("content-type", "application/json");
        if (authorized) req.headers.set("authorization", "Bearer " + token);
        if (!key.empty()) req.headers.set("idempotency-key", key);
        req.body = json_stringify(value);
        return api.handle(req);
    };
    CHECK_EQ(post(review(), "run:1", false).status, 401);
    CHECK_EQ(post(review(), "").status, 400);
    CHECK_EQ(post(review(), "wrong").status, 400);
    const auto result = post(review(), "run:1");
    CHECK_EQ(result.status, 201);
    CHECK((parse_json_or_throw(result.body) == Json{{"ok", true}, {"run_id", "run:1"}}));
    auto retry = review();
    retry["summary"] = "changed on retry";
    CHECK_EQ(post(retry, "run:1").status, 200);
    fail = true;
    CHECK_EQ(post(review("run:2"), "run:2").status, 500);
    CHECK_EQ(voices.db->get("SELECT COUNT(*) AS n FROM human_playtest_voice")->i64("n"), 0);
    const auto section = ai_review_section_for_game(*voices.db, "Bestia");
    CHECK(section.find(ai_review_heading) != std::string::npos);
    CHECK(section.find("試遊のまとめ") != std::string::npos);
    CHECK(section.find("changed on retry") == std::string::npos);
    CHECK(ai_review_section_for_game(*voices.db, "Other").empty());
    std::string paper = "# 議論ペーパー";
    replace_ai_review_section(paper, section);
    const auto first = paper;
    replace_ai_review_section(paper, section);
    CHECK_EQ(paper, first);
    replace_ai_review_section(paper, {});
    CHECK_EQ(paper, "# 議論ペーパー");
}

TEST_CASE("playtest seed uses fresh human and AI voices once, respects cooldown, and leaves other games separate") {
    Voices voices;
    test::TempDir tmp;
    const auto core = core::create_core(tmp.file("kg.kuzu"), tmp.file("events.jsonl"));
    const auto pe = sql::Database::open(":memory:");
    persona_engine::db::apply_persona_engine_migrations(*pe);
    persona_engine::db::PersonasRepo personas(pe);
    discussion_seed::SeedHeadlessDiscussionArgs args;
    args.core = core.get();
    args.personas = &personas;
    args.workspaceId = "knowledge";
    VoiceSeedPolicy policy{2, 10'000};
    index_human_playtest_voices(*voices.db, {{"h1", "Bestia", "人の声"}, {"h1", "Bestia", "再送"}});
    CHECK(voices.save("other-1", "other", "Other"));
    CHECK(!seed_from_playtest_voices(*voices.db, args, policy, 10'000));
    CHECK(voices.save("run:1"));
    const auto seeded = seed_from_playtest_voices(*voices.db, args, policy, 10'000);
    CHECK(seeded);
    const auto gap = core->raw().get("SELECT description, game_id FROM design_gaps WHERE id = ?", {seeded->gapId});
    CHECK_EQ(gap->str("game_id"), "bestia");
    CHECK(gap->str("description").find(ai_review_heading) != std::string::npos);
    CHECK(gap->str("description").find("人の声") != std::string::npos);
    CHECK(!voices.save("run:1"));
    CHECK(voices.save("run:2"));
    CHECK(voices.save("run:3"));
    CHECK(!seed_from_playtest_voices(*voices.db, args, policy, 20'000));
    core->raw().run("UPDATE design_gaps SET status = 'closed' WHERE id = ?", {seeded->gapId});
    CHECK(!seed_from_playtest_voices(*voices.db, args, policy, 19'999));
    CHECK(seed_from_playtest_voices(*voices.db, args, policy, 20'000));
    CHECK(!seed_from_playtest_voices(*voices.db, args, policy, 40'000));
}

TEST_CASE("proposal outbox waits for convergence and retries timeout or missing configuration with the original event") {
    const auto db = sql::Database::open(":memory:");
    db->exec("CREATE TABLE design_gaps (id TEXT PRIMARY KEY, game_id TEXT, evidence_json TEXT, status TEXT)");
    db->run("INSERT INTO design_gaps VALUES (?, ?, ?, ?)", {"gap-1", "bestia", R"({"source":"auto:playtest-voices"})", "open"});
    ensure_proposal_outbox(*db);
    enqueue_playtest_proposal(*db, "gap-1", "改善する", 500);
    enqueue_playtest_proposal(*db, "gap-1", "別の結論", 501);
    std::vector<std::string> recorded;
    const ProposalFetch wire = [&](const http::ClientRequest& req) {
        CHECK_EQ(req.method, "POST");
        CHECK_EQ(req.url, "http://127.0.0.1:11111/v1/management/events");
        CHECK_EQ(req.max_redirects, 0);
        CHECK(!req.headers.get("authorization"));
        recorded.push_back(req.body);
        http::ClientResponse r;
        if (recorded.size() == 1) {
            r.error_kind = http::FetchErrorKind::timeout;
        } else {
            r.transport_ok = true;
            r.status = 200;
            r.body = R"({"event":{"event_key":"di:proposal:gap-1"},"created":false})";
        }
        return r;
    };
    const ProposalDestination configured{"http://127.0.0.1:11111", "Bt"};
    deliver_next_proposal(*db, configured, 500, wire);
    CHECK(recorded.empty());
    db->run("UPDATE design_gaps SET status = 'closed'");
    deliver_next_proposal(*db, {}, 500, wire);
    CHECK_EQ(db->get("SELECT last_error FROM playtest_proposal_outbox")->str("last_error"), "management_url_not_configured");
    deliver_next_proposal(*db, configured, 60'500, wire);
    CHECK_EQ(db->get("SELECT last_error FROM playtest_proposal_outbox")->str("last_error"), "timeout");
    deliver_next_proposal(*db, {configured.baseUrl, "Changed"}, 120'500, wire);
    CHECK_EQ(recorded.size(), 2u);
    CHECK_EQ(recorded[0], recorded[1]);
    const auto payload = parse_json_or_throw(recorded[0]);
    CHECK((payload == Json{{"event_key", "di:proposal:gap-1"}, {"source", "di"}, {"kind", "improvement_proposal"},
                           {"project_code", "Bt"}, {"target_key", "bestia"}, {"origin", "ai"}, {"summary", "改善する"}, {"observed_at", 500}}));
    CHECK_EQ(db->get("SELECT delivered_at FROM playtest_proposal_outbox")->i64("delivered_at"), 120'500);
}

int main() { return di::test::run_all(); }
