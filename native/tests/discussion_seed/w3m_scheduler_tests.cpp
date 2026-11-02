// w3-M: discussion-seed/seed.ts・scheduler.ts の移植の検証 (TS テスト無し。挙動は TS のコードどおり)。
// headless 議論 (gap + session + 進行役の開幕) の形、maxConcurrent、ジャンルの巡回と重複回避、ストアトレンド (偽 Steam)。
#include "check.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include "base/strings.hpp"
#include "core/index.hpp"
#include "core/test_util.hpp"
#include "discussion_seed/scheduler.hpp"
#include "discussion_seed/seed.hpp"
#include "infra/http_server.hpp"
#include "persona_engine/index.hpp"

using namespace di;
using namespace di::discussion_seed;

namespace {

class NullLogger : public persona_engine::Logger {
public:
    void debug(const Json&, std::string_view) override {}
    void info(const Json&, std::string_view m) override { infos.push_back(std::string(m)); }
    void warn(const Json&, std::string_view) override {}
    void error(const Json&, std::string_view) override {}
    std::vector<std::string> infos;
};

struct World {
    test::TempDir dir;
    std::shared_ptr<core::Core> core;
    sql::DatabasePtr pe;
    std::shared_ptr<persona_engine::db::PersonasRepo> personas;
    std::shared_ptr<NullLogger> logger = std::make_shared<NullLogger>();
    World() {
        core = std::shared_ptr<core::Core>(core::create_core(dir.file("kg.kuzu"), dir.file("events.jsonl")).release());
        pe = sql::Database::open(dir.file("pe.db"));
        persona_engine::db::apply_persona_engine_migrations(*pe);
        personas = std::make_shared<persona_engine::db::PersonasRepo>(pe);
    }
    AutoSeedSchedulerDeps deps(AutoSeedConfig cfg) {
        AutoSeedSchedulerDeps d;
        d.core = core;
        d.personas = personas;
        d.workspaceId = "knowledge";
        d.config = std::move(cfg);
        d.logger = logger;
        return d;
    }
};

} // namespace

TEST_CASE("seed_headless_discussion: gap (open, evidence=出自) + session (scene=gap:) + 進行役の開幕") {
    World w;
    SeedHeadlessDiscussionArgs a;
    a.core = w.core.get();
    a.personas = w.personas.get();
    a.workspaceId = "knowledge";
    a.topic = {"お題A", "説明A"};
    a.origin = "auto:genre";
    const auto s = seed_headless_discussion(a);
    const auto gap = w.core->raw().get("SELECT * FROM design_gaps WHERE id = ?", {s.gapId});
    CHECK(gap.has_value());
    CHECK_EQ(gap->str("title"), std::string("お題A"));
    CHECK_EQ(gap->str("description"), std::string("説明A"));
    CHECK_EQ(gap->str("status"), std::string("open"));
    CHECK_EQ(gap->str("evidence_json"), std::string(R"({"source":"auto:genre"})"));
    const auto sess = w.core->raw().get("SELECT * FROM sessions WHERE id = ?", {s.sessionId});
    CHECK_EQ(sess->str("title"), "discussion-of-gap:" + s.gapId);
    CHECK_EQ(sess->str("scene"), "gap:" + s.gapId);
    const auto utt = w.core->raw().get("SELECT * FROM utterances WHERE session_id = ?", {s.sessionId});
    CHECK_EQ(utt->str("speaker_id"), std::string("persona:facilitator"));
    CHECK_EQ(utt->str("raw_content"),
             std::string("今日のお題は「お題A」です。\n説明A\nそれぞれの立場から、 思うところを聞かせてください。 まずは率直な意見からどうぞ。"));
    CHECK(w.personas->get("facilitator").has_value());
    CHECK_EQ(json_stringify(to_json(s)), json_stringify(Json{{"gapId", s.gapId}, {"sessionId", s.sessionId}}));

    // origin 省略は auto:headless、gameId は gap に残る
    a.origin.reset();
    a.gameId = "game-1";
    const auto s2 = seed_headless_discussion(a);
    const auto gap2 = w.core->raw().get("SELECT * FROM design_gaps WHERE id = ?", {s2.gapId});
    CHECK_EQ(gap2->str("evidence_json"), std::string(R"({"source":"auto:headless"})"));
    CHECK_EQ(gap2->str("game_id"), std::string("game-1"));
    SeedHeadlessDiscussionArgs bad;
    CHECK_THROWS(seed_headless_discussion(bad));
}

TEST_CASE("scheduler.trigger: genre を巡回し、同名の open な議題は避け、maxConcurrent で止める") {
    World w;
    auto sched = create_auto_seed_scheduler(w.deps({1000, 2, {}}));  // sources 空 → genre
    const auto a = sched->trigger();
    const auto b = sched->trigger();
    CHECK(a.has_value());
    CHECK(b.has_value());
    CHECK(a->title != b->title);
    CHECK(str::contains(a->title, "何が面白い"));
    // 上限 2 に到達 → skip
    CHECK(!sched->trigger().has_value());
    CHECK_EQ(w.logger->infos.back(), std::string("auto-seed skipped (maxConcurrent reached)"));
    // 1 つ closed にすると枠が空く (converged / closed は数えない。dismissed は数える = TS と同じ)
    w.core->raw().run("UPDATE design_gaps SET status = 'closed' WHERE id = ?", {a->gapId});
    const auto c = sched->trigger();
    CHECK(c.has_value());
    w.core->raw().run("UPDATE design_gaps SET status = 'dismissed' WHERE id = ?", {b->gapId});
    CHECK(!sched->trigger().has_value());
    // evidence は auto:genre
    CHECK_EQ(w.core->raw().get("SELECT evidence_json FROM design_gaps WHERE id = ?", {c->gapId})->str("evidence_json"),
             std::string(R"({"source":"auto:genre"})"));
}

TEST_CASE("scheduler.trigger: 17 ジャンルすべて open なら no fresh topic") {
    World w;
    auto sched = create_auto_seed_scheduler(w.deps({1000, 100, {"genre"}}));
    int made = 0;
    for (int i = 0; i < 40; ++i)
        if (sched->trigger()) ++made;
    CHECK_EQ(made, 17);
    CHECK_EQ(w.logger->infos.back(), std::string("auto-seed skipped (no fresh topic)"));
}

TEST_CASE("scheduler.trigger: store-trend は偽 Steam の作品から立て、取得失敗はフォールバック") {
    World w;
    auto router = std::make_shared<http::Router>();
    std::atomic<bool> fail{false};
    router->get("/api/featuredcategories/", [&](http::Request&) {
        if (fail) return http::Response::text("err", 500);
        return http::Response::bytes(R"({"top_sellers":{"items":[{"name":"架空ゲームA"},{"name":"架空ゲームB"}]}})", "application/json");
    });
    http::ServerOptions o;
    o.port = 0;
    http::Server server(o, router);
    server.start();
    auto d = w.deps({1000, 10, {"store-trend", "genre"}});
    d.storeTrendSource.url = "http://127.0.0.1:" + std::to_string(server.bound_port()) + "/api/featuredcategories/";
    d.storeTrendSource.allow_private_network = true;
    auto sched = create_auto_seed_scheduler(d);
    const auto a = sched->trigger();  // store-trend
    CHECK(a.has_value());
    CHECK(str::contains(a->title, "架空ゲームA"));
    CHECK_EQ(w.core->raw().get("SELECT evidence_json FROM design_gaps WHERE id = ?", {a->gapId})->str("evidence_json"),
             std::string(R"({"source":"auto:store-trend"})"));
    const auto b = sched->trigger();  // genre
    CHECK(b.has_value() && str::contains(b->title, "何が面白い"));
    const auto c = sched->trigger();  // store-trend: A は open なので B
    CHECK(c.has_value() && str::contains(c->title, "架空ゲームB"));
    fail = true;
    sched->trigger();                  // genre
    const auto e = sched->trigger();  // store-trend 失敗 → 静的フォールバックの作品
    CHECK(e.has_value());
    server.stop();
}

TEST_CASE("scheduler.start/stop: 定期で種をまき、stop 後は増えない") {
    World w;
    auto sched = create_auto_seed_scheduler(w.deps({10, 3, {"genre"}}));
    sched->start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto count = [&] { return w.core->raw().get("SELECT COUNT(*) AS n FROM design_gaps")->i64("n"); };
    while (count() < 3 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched->stop();
    CHECK_EQ(count(), 3);  // maxConcurrent で止まる
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(count(), 3);
}

DI_TEST_MAIN()
