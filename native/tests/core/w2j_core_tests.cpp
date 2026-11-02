// w2-J: core の組み立て (createCore)・EventLog・リポジトリ・ベクトル・クエリ埋め込みキャッシュ・KG レジストリ。
// tests/core/run.ts, tests/core/kg-registry.test.ts, tests/core/query-embed-cache.test.ts の移植 + 境界。
#include <atomic>
#include <chrono>
#include <thread>

#include "check.hpp"
#include "core/test_util.hpp"

#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"
#include "core/vectors/query_embed_cache.hpp"
#include "fundamentum_export/store.hpp"

using namespace di;
using namespace di::core;

TEST_CASE("createCore: migration 再実行・repos・jsonl・utterance の不変条件・vector・rebuild (run.ts)") {
    test::TempDir dir;
    const std::string db = dir.file("discatier.kuzu");
    const std::string events = dir.file("events.jsonl");
    { create_core(db, events)->close(); }  // migration re-run should be no-op
    auto core = create_core(db, events);
    const std::string ws = "ws-1";
    const auto person_id = core->repos.person.create({std::nullopt, ws, "alice", std::nullopt});
    CHECK(!person_id.empty());
    const auto session_id = core->repos.session.create({std::nullopt, ws, "s1", static_cast<double>(time::now_ms()), std::nullopt, std::nullopt, std::nullopt});
    repositories::UtteranceInput u;
    u.workspaceId = ws;
    u.sessionId = session_id;
    u.rawContent = "hello";
    u.postedAt = static_cast<double>(time::now_ms());
    const auto utt_id = core->repos.utterance.create(u);

    const auto lines = str::split(str::trim(*fs::read_file(events)), "\n");
    CHECK(lines.size() >= 3);
    // jsonl の 1 行は {id, eventType, payload, createdAt}。payload の id は末尾 (`{ ...input, id }`)。
    const Json first = parse_json_or_throw(lines[0]);
    CHECK_EQ(json_stringify(first["payload"]), json_stringify(Json{{"workspaceId", ws}, {"name", "alice"}, {"id", person_id}}));
    CHECK_EQ(first["eventType"].get<std::string>(), std::string("PersonCreated"));

    auto row = core->repos.utterance.get(utt_id);
    CHECK(row && row->str("raw_content") == "hello");
    core->repos.utterance.update(utt_id, {ws, "hello!", std::nullopt});
    CHECK_THROWS(core->repos.utterance.update(utt_id, {ws, "x", std::string("rewritten")}));
    core->repos.utterance.update(utt_id, {ws, "same", std::string("hello")});  // 同じ raw は許す
    CHECK_THROWS(core->repos.utterance.update("missing", {ws, "x", std::nullopt}));

    core->register_embedding({ws, "utterance", utt_id, {1, 0, 0}});
    core->register_embedding({ws, "utterance", "u2", {0, 1, 0}});
    const auto sims = core->search_similar({ws, {0.9, 0.1, 0}, 1, std::nullopt});
    CHECK_EQ(sims.size(), std::size_t{1});
    CHECK_EQ(sims[0].nodeId, utt_id);
    CHECK_THROWS(core->register_embedding({ws, "utterance", "bad", {}}));
    CHECK_THROWS(core->register_embedding({ws, "utterance", "bad", {1, std::nan("")}}));

    core->eventLog.rebuild_from_event_store();
    CHECK_EQ(core->repos.session.list(ws).size(), std::size_t{1});
    CHECK_EQ(core->repos.person.list(ws).size(), std::size_t{1});
    core->close();
}

TEST_CASE("repos: games/aesthetics は直接書き、game は PlayContextCaptured も記録する") {
    test::TempDir dir;
    auto core = create_core(dir.file("kg.sqlite"), dir.file("ev.jsonl"));
    const auto game = core->repos.game.create({std::string("g1"), "ws", "Title", std::nullopt});
    CHECK_EQ(game, std::string("g1"));
    CHECK(core->repos.game.get("g1")->is_null("genre"));
    core->repos.game.update("g1", {"ws", "T2", std::string("rpg")});
    CHECK_EQ(core->repos.game.get("g1")->str("genre"), std::string("rpg"));
    const auto events = core->eventLog.load_events_since();
    CHECK_EQ(events.size(), std::size_t{1});
    CHECK_EQ(events[0].eventType, std::string("PlayContextCaptured"));
    CHECK_EQ(events[0].payload["gameId"].get<std::string>(), std::string("g1"));
    core->repos.aesthetic.create({std::nullopt, "ws", std::nullopt, "美", std::string("d")});
    CHECK_EQ(core->repos.aesthetic.list("ws").size(), std::size_t{1});
    // 呼び出し側が id を渡したときは payload の先頭
    core->repos.designGap.create({std::string("gap-1"), "ws", std::nullopt, "t", std::nullopt, std::nullopt});
    const auto all = core->eventLog.load_events_since();
    CHECK_EQ(json_stringify(all.back().payload), json_stringify(Json{{"id", "gap-1"}, {"workspaceId", "ws"}, {"title", "t"}}));
    CHECK(core->repos.designGap.get("gap-1").has_value());
    // eventId 起点の読み出し
    CHECK_EQ(core->eventLog.load_events_since({all.back().id, std::nullopt}).size(), std::size_t{1});
}

TEST_CASE("EventLog: 1 行の上限ちょうど/超過・壊れた payload") {
    test::TempDir dir;
    auto core = create_core(dir.file("kg.sqlite"), dir.file("ev.jsonl"));
    events::AppendEventInput in;
    in.eventType = "Custom";
    in.id = "e1";
    in.createdAt = 1;
    // {"id":"e1","eventType":"Custom","payload":{"t":"..."},"createdAt":1}\n の大きさを上限ちょうどにする
    const std::string skeleton = json_stringify(Json{{"id", "e1"}, {"eventType", "Custom"}, {"payload", Json{{"t", ""}}}, {"createdAt", 1}}) + "\n";
    in.payload = Json{{"t", std::string(events::kMaxEventLineBytes - skeleton.size(), 'a')}};
    core->eventLog.append_event(in);
    in.id = "e2";
    in.payload = Json{{"t", std::string(events::kMaxEventLineBytes - skeleton.size() + 1, 'a')}};
    try {
        core->eventLog.append_event(in);
        CHECK(false);
    } catch (const std::length_error&) {
    }
    CHECK(!core->raw().get("SELECT id FROM events WHERE id = 'e2'").has_value());  // DB にも書かない
    CHECK_EQ(core->eventLog.load_events_since().size(), std::size_t{1});
    core->raw().run("INSERT INTO events (id, event_type, payload_json, created_at) VALUES ('bad', 'X', '{broken', 5)");
    CHECK_THROWS(core->eventLog.load_events_since());
}

TEST_CASE("vector_search: 壊れた行・非有限・巨大な vector_json は外す / k の境界 / 500 件ごとの分割") {
    test::TempDir dir;
    auto core = create_core(dir.file("kg.sqlite"), dir.file("ev.jsonl"));
    auto& db = core->raw();
    for (int i = 0; i < 1203; ++i) core->register_embedding({"ws", "voice", "n" + std::to_string(i), {1, static_cast<double>(i)}});
    db.run("INSERT INTO embeddings (id, workspace_id, node_type, node_id, vector_json, created_at) VALUES ('x1','ws','voice','broken','[1,',1)");
    db.run("INSERT INTO embeddings (id, workspace_id, node_type, node_id, vector_json, created_at) VALUES ('x2','ws','voice','strs','[\"a\"]',1)");
    db.run("INSERT INTO embeddings (id, workspace_id, node_type, node_id, vector_json, created_at) VALUES ('x3','ws','voice','huge',?,1)",
           {"[" + str::repeat("1,", 2'100'000) + "1]"});
    std::vector<std::string> ids{"broken", "strs", "huge", "absent"};
    for (int i = 0; i < 1203; ++i) ids.push_back("n" + std::to_string(i));
    const auto got = vectors::fetch_vectors_by_node_ids(db, {"ws", "voice", ids});
    CHECK_EQ(got.size(), std::size_t{1203});
    CHECK(!got.count("broken") && !got.count("huge"));
    CHECK_EQ(vectors::search_similar(db, {"ws", {1, 0}, 0, std::nullopt}).size(), std::size_t{0});
    CHECK_EQ(vectors::search_similar(db, {"ws", {1, 0}, -3, std::nullopt}).size(), std::size_t{0});
    CHECK_EQ(vectors::search_similar(db, {"ws", {1, 0}, 2.7, std::string("voice")}).size(), std::size_t{2});
    CHECK_EQ(vectors::search_similar(db, {"ws", {1, 0}, 1e9, std::string("voice")}).size(), std::size_t{1203});
    CHECK_EQ(vectors::search_similar(db, {"ws", {1, 0}, 5, std::string("other")}).size(), std::size_t{0});
    CHECK_EQ(vectors::search_similar(db, {"ws", {1, 0}, 1, std::nullopt})[0].nodeId, std::string("n0"));
}

namespace {
struct FakeEmbedder : vectors::EmbeddingClient {
    std::atomic<int> calls{0};
    std::vector<double> reply{0.25, 0.75};
    int delay_ms = 0;
    bool fail = false;
    std::vector<std::vector<double>> embed(const std::vector<std::string>&) override {
        ++calls;
        if (delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        if (fail) throw std::runtime_error("down");
        return {reply};
    }
};
} // namespace

TEST_CASE("query-embed-cache: 呼び出し元が DB を閉じても永続化・本文を保存しない・多重発火の抑止") {
    vectors::reset_query_embed_cache_for_test();
    test::TempDir dir;
    const std::string path = dir.file("cache.sqlite");
    auto db = sql::Database::open(path);
    auto embedder = std::make_shared<FakeEmbedder>();
    embedder->delay_ms = 50;
    std::vector<std::string> warnings;
    std::mutex wm;
    auto warn = [&](const std::string& m) {
        std::lock_guard l(wm);
        warnings.push_back(m);
    };
    auto f1 = vectors::warm_query_embedding(db, embedder, "test-model", "秘密の議題", warn);
    auto f2 = vectors::warm_query_embedding(db, embedder, "test-model", "秘密の議題", warn);
    db->close();
    f1.wait();
    f2.wait();
    CHECK_EQ(embedder->calls.load(), 1);
    vectors::reset_query_embed_cache_for_test();
    auto reopened = sql::Database::open(path);
    const auto vec = vectors::get_cached_query_vector(*reopened, "test-model", "秘密の議題");
    CHECK(vec && *vec == (std::vector<double>{0.25, 0.75}));
    CHECK_EQ(reopened->get("SELECT query_text FROM embedding_query_cache")->str("query_text"), std::string(""));
    CHECK(warnings.empty());
    // 別モデルはキャッシュが割れる
    CHECK(!vectors::get_cached_query_vector(*reopened, "other-model", "秘密の議題"));
    // 失敗は warn して投げない
    embedder->fail = true;
    embedder->delay_ms = 0;
    vectors::warm_query_embedding_sync(reopened, embedder, "m", "q2", warn);
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK(str::contains(warnings[0], "キーワード検索で継続"));
    // 非有限ベクトルは保存しない
    embedder->fail = false;
    embedder->reply = {1, std::numeric_limits<double>::infinity()};
    vectors::warm_query_embedding_sync(reopened, embedder, "m", "q3", warn);
    CHECK_EQ(warnings.size(), std::size_t{2});
    CHECK(!vectors::get_cached_query_vector(*reopened, "m", "q3"));
    // 旧行の query_text は初回に消す・壊れた行は無いものとして扱う
    reopened->run("INSERT INTO embedding_query_cache VALUES (?, 'old topic', '[1,2]', 1)", {vectors::hash_query("m", "q4")});
    reopened->run("INSERT INTO embedding_query_cache VALUES (?, '', '{bad', 1)", {vectors::hash_query("m", "q5")});
    CHECK(!vectors::get_cached_query_vector(*reopened, "m", "q5"));
    auto third = sql::Database::open(dir.file("cache2.sqlite"));
    vectors::ensure_query_cache_table(*third);
    third->run("INSERT INTO embedding_query_cache VALUES ('h', 'leak', '[1]', 1)");
    third->close();
    auto fourth = sql::Database::open(dir.file("cache2.sqlite"));
    vectors::reset_query_embed_cache_for_test();
    CHECK(!vectors::get_cached_query_vector(*fourth, "m", "zz"));
}

TEST_CASE("query-embed-cache: 行数の上限 (1000) を超えたら古い順に消す") {
    vectors::reset_query_embed_cache_for_test();
    test::TempDir dir;
    auto db = sql::Database::open(dir.file("c.sqlite"));
    vectors::ensure_query_cache_table(*db);
    for (int i = 0; i < 1000; ++i)
        db->run("INSERT INTO embedding_query_cache VALUES (?, '', '[1]', ?)", {"h" + std::to_string(i), i});
    auto embedder = std::make_shared<FakeEmbedder>();
    vectors::warm_query_embedding_sync(db, embedder, "m", "new", {});
    CHECK_EQ(db->get("SELECT COUNT(*) AS n FROM embedding_query_cache")->i64("n"), 1000);
    CHECK(!db->get("SELECT 1 FROM embedding_query_cache WHERE query_hash = 'h0'").has_value());
}

TEST_CASE("normalize_query_text (golden)") {
    for (const auto& c : test::load_fixture("w2j/normalize_query.json")) {
        std::vector<std::string> terms;
        for (const auto& t : c["terms"]) terms.push_back(t.get<std::string>());
        CHECK_EQ(vectors::normalize_query_text(terms), c["out"].get<std::string>());
    }
}

namespace {
config::DiscutereConfig cfg() { return config::DiscutereConfig{}; }
config::KnowledgeGraphEntry kg(std::string id, std::optional<std::string> label, std::string path) {
    return config::KnowledgeGraphEntry{std::move(id), std::move(label), std::move(path)};
}
} // namespace

TEST_CASE("kg-registry (kg-registry.test.ts)") {
    auto reg = cfg();
    reg.discatier.activeKg = "hollow";
    reg.discatier.knowledgeGraphs = std::vector<config::KnowledgeGraphEntry>{kg("default", "総合", "./data/kg/default.kuzu"),
                                                                             kg("hollow", "Hollow Knight", "./data/kg/hollow.kuzu")};
    CHECK_EQ(*resolve_active_kg_path(reg), std::string("./data/kg/hollow.kuzu"));
    auto graphs = list_knowledge_graphs(reg);
    CHECK_EQ(graphs.size(), std::size_t{2});
    CHECK(graphs[1].active && !graphs[0].active);
    CHECK_EQ(json_stringify(to_json(graphs[0])),
             json_stringify(Json{{"id", "default"}, {"label", "総合"}, {"kuzuPath", "./data/kg/default.kuzu"}, {"active", false}}));

    auto no_active = cfg();
    no_active.discatier.knowledgeGraphs = std::vector<config::KnowledgeGraphEntry>{kg("default", std::nullopt, "./data/kg/default.kuzu"),
                                                                                   kg("x", std::nullopt, "./data/kg/x.kuzu")};
    CHECK_EQ(*resolve_active_kg_path(no_active), std::string("./data/kg/default.kuzu"));
    CHECK_EQ(list_knowledge_graphs(no_active)[1].label, std::string("x"));

    auto mismatch = cfg();
    mismatch.discatier.activeKg = "nope";
    mismatch.discatier.knowledgeGraphs = std::vector<config::KnowledgeGraphEntry>{kg("first", std::nullopt, "./data/kg/first.kuzu"),
                                                                                  kg("second", std::nullopt, "./b.kuzu")};
    CHECK_EQ(*resolve_active_kg_path(mismatch), std::string("./data/kg/first.kuzu"));
    CHECK(list_knowledge_graphs(mismatch)[0].active);

    auto legacy = cfg();
    legacy.discatier.kuzuPath = "./data/discatier.kuzu";
    CHECK_EQ(*resolve_active_kg_path(legacy), std::string("./data/discatier.kuzu"));
    const auto ll = list_knowledge_graphs(legacy);
    CHECK(ll.size() == 1 && ll[0].id == "default" && ll[0].active && ll[0].label == "総合");

    auto empty = cfg();
    CHECK(!resolve_active_kg_path(empty));
    CHECK(list_knowledge_graphs(empty).empty());
    CHECK_EQ(resolve_active_kg_dir(empty), fs::resolve("./data/external"));
    CHECK_EQ(resolve_active_kg_dir(reg), fs::resolve("./data/kg"));
    // 空の knowledgeGraphs は旧 kuzuPath へ
    auto empty_list = legacy;
    empty_list.discatier.knowledgeGraphs = std::vector<config::KnowledgeGraphEntry>{};
    CHECK_EQ(*resolve_active_kg_path(empty_list), std::string("./data/discatier.kuzu"));
}

TEST_CASE("fundamentum store: シングルトンの差し替え") {
    auto mem = std::shared_ptr<lib::fundamentum::Foundation>(lib::fundamentum::Foundation::in_memory());
    fundamentum_export::set_fundamentum_store_for_tests(mem);
    CHECK(&fundamentum_export::get_fundamentum_store() == mem.get());
    CHECK(&fundamentum_export::get_fundamentum_store() == mem.get());
    fundamentum_export::set_fundamentum_store_for_tests(nullptr);
}

DI_TEST_MAIN()
