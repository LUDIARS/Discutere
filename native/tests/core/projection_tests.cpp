// イベント射影 (src/core/events/projection.ts) の golden: Node で射影した後の各テーブルの行と一致する。
#include "core/test_util.hpp"

#include "core/db/js_bind.hpp"
#include "core/db/schema.hpp"
#include "core/events/projection.hpp"

using namespace di;
using namespace di::core;

TEST_CASE("golden: projected rows match Node") {
    const Json fx = test::load_fixture("core/projection.json");
    auto db = sql::Database::open(":memory:");
    core::db::apply_migrations(*db);
    for (const auto& e : fx["events"]) events::apply_event_projection(*db, events::domain_event_from_json(e));
    for (auto it = fx["rows"].begin(); it != fx["rows"].end(); ++it) {
        const auto actual = test::rows_json(*db, "SELECT * FROM " + it.key() + " ORDER BY id");
        if (json_stringify(actual) != json_stringify(it.value()))
            throw test::Failure(it.key() + "\n  actual:   " + json_stringify(actual) + "\n  expected: " + json_stringify(it.value()));
    }
}

TEST_CASE("payload values that better-sqlite3 cannot bind are rejected") {
    auto db = sql::Database::open(":memory:");
    core::db::apply_migrations(*db);
    events::DomainEvent e;
    e.id = "e";
    e.eventType = std::string(events::EVENT_TYPES::PersonCreated);
    e.payload = {{"workspaceId", "w"}, {"id", "p"}, {"name", true}};
    CHECK_THROWS(events::apply_event_projection(*db, e));
    e.payload["name"] = Json::object();
    CHECK_THROWS(events::apply_event_projection(*db, e));
    CHECK_THROWS(events::domain_event_from_json(Json::array()));
    CHECK_THROWS(events::domain_event_from_json(Json{{"id", 1}, {"eventType", "x"}, {"payload", Json::object()}}));
}

TEST_CASE("payload to_json keeps TS key order and omits absent optionals") {
    events::SessionPayload s;
    s.workspaceId = "w";
    s.id = "s";
    s.title = "t";
    s.startedAt = 5;
    s.scene = "web:r";
    CHECK_EQ(json_stringify(events::to_json(s)), std::string(R"({"workspaceId":"w","id":"s","title":"t","startedAt":5,"scene":"web:r"})"));
    CHECK(events::is_event_type("AffectMeasured"));
    CHECK(!events::is_event_type("Nope"));
}

DI_TEST_MAIN()
