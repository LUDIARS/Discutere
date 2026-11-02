// src/flow/persona-history-evidence.ts の移植の確認 (tests/crawler/comment-history.test.ts の該当観点)。
// flow_persona / flow_persona_history はテスト内で合成する。
#include "check.hpp"

#include "base/json.hpp"
#include "flow/persona_history_evidence.hpp"

using namespace di;
using namespace di::flow;

namespace {
const double NOW = 1'800'000'000'000.0;
Json valid() {
    return Json{{"schemaVersion", 1}, {"source", "steam"}, {"generatedAt", NOW - 1000}, {"expiresAt", NOW + 1000},
                {"windowStart", NOW - 9000}, {"firstObservedAt", NOW - 2000}, {"lastObservedAt", NOW - 2000},
                {"sampleCount", 10}, {"observedMonths", 1}, {"coverage", "partial"}};
}
} // namespace

TEST_CASE("valid evidence parses and keeps the schema key order") {
    const auto e = parse_history_evidence(valid(), NOW);
    CHECK(e.has_value());
    CHECK_EQ(json_stringify(to_json(*e)), json_stringify(valid()));
    CHECK(!parse_history_evidence(std::nullopt, NOW).has_value());
}

TEST_CASE("expired / inconsistent evidence is rejected") {
    Json expired = valid();
    expired["expiresAt"] = NOW;
    try {
        parse_history_evidence(expired, NOW);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("expired") != std::string::npos);
    }
    for (auto patch : {Json{{"generatedAt", NOW + 1}}, Json{{"expiresAt", NOW - 1000 + 31.0 * 86400000}},
                       Json{{"windowStart", NOW - 1500}}, Json{{"firstObservedAt", NOW - 1}}, Json{{"lastObservedAt", NOW - 500}}}) {
        Json j = valid();
        for (auto it = patch.begin(); it != patch.end(); ++it) j[it.key()] = it.value();
        CHECK_THROWS(parse_history_evidence(j, NOW));
    }
}

TEST_CASE("strict schema: unknown keys, wrong types, ranges") {
    for (auto patch : {Json{{"extra", 1}}, Json{{"schemaVersion", 2}}, Json{{"source", "twitter"}}, Json{{"sampleCount", 0}},
                       Json{{"sampleCount", 1.5}}, Json{{"observedMonths", 8}}, Json{{"observedMonths", 0}},
                       Json{{"coverage", "full"}}, Json{{"generatedAt", "x"}}}) {
        Json j = valid();
        for (auto it = patch.begin(); it != patch.end(); ++it) j[it.key()] = it.value();
        CHECK_THROWS(parse_history_evidence(j, NOW));
    }
    Json missing = valid();
    missing.erase("coverage");
    CHECK_THROWS(parse_history_evidence(missing, NOW));
    CHECK_THROWS(parse_history_evidence(Json::array(), NOW));
}

TEST_CASE("save upserts, purge removes only expired derived profiles") {
    auto db = sql::Database::open(":memory:");
    db->exec("CREATE TABLE flow_persona(user_id TEXT PRIMARY KEY);"
             "CREATE TABLE flow_persona_history(user_id TEXT PRIMARY KEY, expires_at INTEGER, payload TEXT);"
             "INSERT INTO flow_persona VALUES('expired'),('fresh'),('registered');");
    db->run("INSERT INTO flow_persona_history VALUES(?,?,?)", {"expired", NOW, "{}"});
    save_history_evidence(*db, "fresh", parse_history_evidence(valid(), NOW));
    save_history_evidence(*db, "fresh", parse_history_evidence(valid(), NOW));  // 2 回目は更新
    save_history_evidence(*db, "none", std::nullopt);
    const auto row = db->get("SELECT expires_at, payload FROM flow_persona_history WHERE user_id='fresh'");
    CHECK_EQ(row->num("expires_at"), NOW + 1000);
    CHECK_EQ(row->str("payload"), json_stringify(valid()));
    purge_history_personas(*db, NOW);
    Json ids = Json::array();
    for (const auto& r : db->all("SELECT user_id FROM flow_persona ORDER BY user_id")) ids.push_back(r.str("user_id"));
    CHECK_EQ(json_stringify(ids), std::string(R"(["fresh","registered"])"));
    CHECK_EQ(db->all("SELECT * FROM flow_persona_history").size(), std::size_t{1});
    purge_history_personas(*db, NOW);  // 期限切れが無ければ何もしない
}

DI_TEST_MAIN()
