// @ludiars/blackbox store-sqlite.test.ts の移植 (:memory: の SQLite)。
#include "check.hpp"

#include "lib/blackbox/blackbox.hpp"

using namespace di;
using namespace di::lib::blackbox;

namespace {
const Json COND = Json{{"op", "cmp"}, {"feature", "x"}, {"cmp", ">="}, {"value", 1}};
sql::DatabasePtr fresh_db() { return sql::Database::open(":memory:"); }
} // namespace

TEST_CASE("insert / get / update / findByFingerprint round-trip") {
    auto db = fresh_db();
    ensure_blackbox_schema(*db);
    SqliteRuleStore store(db);
    RuleDraft d;
    d.domain = "d";
    d.description = "r1";
    d.when = COND;
    d.output = Json{{"y", 1}};
    auto rule = store.insert(d);
    CHECK_EQ(rule.state, std::string("candidate"));
    CHECK(!rule.fingerprint.empty());
    CHECK_EQ(store.find_by_fingerprint("d", rule.fingerprint)->id, rule.id);
    RulePatch p;
    p.state = "trial";
    p.shadowAgreements = 3;
    auto updated = store.update(rule.id, p);
    CHECK_EQ(updated->state, std::string("trial"));
    CHECK_EQ(updated->shadowAgreements, std::int64_t(3));
    CHECK_EQ(store.list_by_domain("d").size(), std::size_t(1));
    CHECK_EQ(json_stringify(updated->when), json_stringify(COND));
    CHECK(!store.update("missing", p));
}

TEST_CASE("ledger record / verdict / listPending / listRecent + shadow_json persistence") {
    auto db = fresh_db();
    ensure_blackbox_schema(*db);
    SqliteDecisionLedger ledger(db);
    DecisionInput in;
    in.domain = "d";
    in.input = Json{{"a", 1}};
    in.features = Json{{"x", 2}};
    in.output = Json{{"y", 1}};
    in.source = "llm";
    in.confidence = 0.9;
    in.rationale = "t";
    in.status = "pending_review";
    in.shadow = {{"r-1", true}};
    in.createdAt = "2026-07-02T00:00:00.000Z";
    auto id = ledger.record(in);
    auto rec = ledger.get(id);
    CHECK((rec->shadow == std::vector<ShadowEval>{{"r-1", true}}));
    CHECK_EQ(json_stringify(to_json(*rec)),
             std::string(R"({"id":1,"domain":"d","input":{"a":1},"features":{"x":2},"output":{"y":1},"source":"llm","ruleId":null,"confidence":0.9,"rationale":"t","status":"pending_review","verdict":null,"shadow":[{"ruleId":"r-1","agreed":true}],"createdAt":"2026-07-02T00:00:00.000Z","reviewedAt":null})"));
    CHECK_EQ(ledger.list_pending(std::string("d")).size(), std::size_t(1));
    ledger.set_verdict(id, "ok", "2026-07-02T00:01:00.000Z");
    CHECK(ledger.list_pending(std::string("d")).empty());
    CHECK_EQ(ledger.list_recent("d", 10).size(), std::size_t(1));
    // 保存した JSON 列の形 (TS と同じ JSON.stringify)。
    auto row = db->get("SELECT input_json, features_json, shadow_json FROM blackbox_decisions WHERE id = ?", {id});
    CHECK_EQ(row->str("input_json"), std::string(R"({"a":1})"));
    CHECK_EQ(row->str("shadow_json"), std::string(R"([{"ruleId":"r-1","agreed":true}])"));
}

TEST_CASE("engine on SQLite stores learns up to promote") {
    auto bb = make_sqlite_blackbox(fresh_db());
    int calls = 0;
    for (int i = 0; i < 4; ++i) {
        bb.engine->decide("d", Json::object(), Json{{"x", 5}}, [&](const Json&, const Json&, const LlmContext&) {
            calls += 1;
            LlmJudgement j;
            j.output = Json{{"y", 1}};
            j.confidence = 0.9;
            j.rationale = "t";
            j.proposedRule = RuleProposal{"x>=1 なら y=1", COND, Json{{"y", 1}}, std::nullopt, std::nullopt};
            return j;
        });
    }
    CHECK_EQ(bb.rules->list_by_domain("d")[0].state, std::string("trial"));
    auto s = bb.stats("d");
    CHECK_EQ(s.window, std::int64_t(4));
    CHECK_EQ(s.llmDecisions, std::int64_t(4));
    CHECK_EQ(s.ruleStates.trial, std::int64_t(1));
}

TEST_CASE("migrates Memoria old schema (enabled column / no state)") {
    auto db = fresh_db();
    db->exec(R"(
      CREATE TABLE blackbox_rules (
        id TEXT PRIMARY KEY, domain TEXT NOT NULL, description TEXT NOT NULL,
        when_json TEXT NOT NULL, output_json TEXT NOT NULL,
        confidence REAL NOT NULL, enabled INTEGER NOT NULL,
        source TEXT NOT NULL, approvals INTEGER NOT NULL, rejections INTEGER NOT NULL,
        priority INTEGER NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL
      );
      CREATE TABLE blackbox_decisions (
        id INTEGER PRIMARY KEY AUTOINCREMENT, domain TEXT NOT NULL,
        input_json TEXT, features_json TEXT, output_json TEXT,
        source TEXT NOT NULL, rule_id TEXT, confidence REAL NOT NULL,
        rationale TEXT NOT NULL, status TEXT NOT NULL, verdict TEXT,
        created_at TEXT NOT NULL, reviewed_at TEXT
      );
    )");
    const std::string when = json_stringify(COND);
    const std::string ins = R"(INSERT INTO blackbox_rules VALUES (?, 'd', ?, ?, '{"y":1}', 0.7, ?, 'llm', ?, ?, 0, 't', 't'))";
    db->run(ins, {"r-cand", "未承認 LLM 提案", when, 0, 0, 0});
    db->run(ins, {"r-trial", "有効化済み", when, 1, 1, 0});
    db->run(ins, {"r-auto", "承認3回", when, 1, 3, 0});
    db->run(ins, {"r-retired", "NG3回", when, 0, 0, 3});

    ensure_blackbox_schema(*db);
    SqliteRuleStore store(db);
    std::map<std::string, Rule> by_id;
    for (auto& r : store.list_by_domain("d")) by_id[r.id] = r;
    CHECK_EQ(by_id["r-cand"].state, std::string("candidate"));
    CHECK_EQ(by_id["r-trial"].state, std::string("trial"));
    CHECK_EQ(by_id["r-auto"].state, std::string("auto"));
    CHECK_EQ(by_id["r-retired"].state, std::string("retired"));
    CHECK_EQ(by_id["r-cand"].fingerprint, by_id["r-trial"].fingerprint);
    CHECK_EQ(by_id["r-cand"].fingerprint, rule_fingerprint(COND, Json{{"y", 1}}));
    ensure_blackbox_schema(*db);
    CHECK_EQ(store.list_by_domain("d").size(), std::size_t(4));
    // 旧 decisions に shadow_json が足されている。
    auto cols = db->table_columns("blackbox_decisions");
    CHECK(std::find(cols.begin(), cols.end(), "shadow_json") != cols.end());
}

TEST_CASE("corrupt / oversized JSON columns are rejected with an error (not trusted)") {
    auto db = fresh_db();
    ensure_blackbox_schema(*db);
    db->run(R"(INSERT INTO blackbox_rules (id, domain, description, when_json, output_json, created_at, updated_at, fingerprint)
               VALUES ('bad', 'd', 'x', '{broken', 'null', 't', 't', 'f'))");
    SqliteRuleStore store(db);
    CHECK_THROWS(store.get("bad"));
    // 上限 (1 MiB) ちょうどは読める / +1 は拒否。
    auto limit = db_json_limits().max_bytes;
    std::string exact = "\"" + std::string(limit - 2, 'a') + "\"";
    std::string over = "\"" + std::string(limit - 1, 'a') + "\"";
    db->run("UPDATE blackbox_rules SET when_json = ? WHERE id = 'bad'", {exact});
    CHECK_EQ(store.get("bad")->when.get<std::string>().size(), limit - 2);
    db->run("UPDATE blackbox_rules SET when_json = ? WHERE id = 'bad'", {over});
    CHECK_THROWS(store.get("bad"));
}

DI_TEST_MAIN()
