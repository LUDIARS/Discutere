// w3l_conclusions のテスト (3/3)。準備は w3l_conclusions_support.hpp。
#include "w3l_conclusions_support.hpp"

using namespace w3l_conclusions_support;

TEST_CASE("境界: DB の JSON 列 (上限ちょうど / 上限 + 1 / 壊れた / 深い入れ子)") {
    TempDir dir;
    auto db = flow_db(dir.path + "/b.db");
    auto insert = [&](const std::string& sid, const std::string& auf) {
        db->run("INSERT INTO discussion_paper (id, flow, session_id, theme, tags_json, created_at, updated_at) VALUES (?, 'discussion', ?, 't', ?, 1, 1)",
                {"p-" + sid, sid, auf});
        db->run("INSERT INTO flow_conclusion (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at) VALUES (?, ?, '【収束】x', ?, '[]', 1, 1)",
                {sid, "p-" + sid, auf});
    };
    // ちょうど上限 (4MiB) の配列は読める
    std::string exact = "[\"" + std::string(kFlowConclusionJsonMaxBytes - 4, 'a') + "\"]";
    CHECK_EQ(exact.size(), kFlowConclusionJsonMaxBytes);
    insert("exact", exact);
    insert("over", "[\"" + std::string(kFlowConclusionJsonMaxBytes - 3, 'a') + "\"]");
    insert("broken", "[\"a\",");
    insert("deep", std::string(10'000, '[') + std::string(10'000, ']'));
    CHECK_EQ(get_flow_conclusion_detail(*db, "exact")->aufhebungCount, 1);
    CHECK_EQ(get_flow_conclusion_detail(*db, "exact")->paper->tags.size(), 1u);
    CHECK_EQ(get_flow_conclusion_detail(*db, "over")->aufhebungCount, 0);
    CHECK_EQ(get_flow_conclusion_detail(*db, "broken")->aufhebungCount, 0);
    CHECK_EQ(get_flow_conclusion_detail(*db, "deep")->aufhebungCount, 0);
}

TEST_CASE("golden: conclusions (KG の design_gaps 経路・話者マスク・出所)") {
    TempDir dir;
    auto core = core::create_core(dir.path + "/kg.sqlite", dir.path + "/events.jsonl");
    const Json& g = w3l::golden()["kgConclusions"];
    for (const auto& s : g["setupSql"]) core->raw().exec(s.get<std::string>());
    W3L_SAME(summaries_json(list_conclusions(*core, "ws", 20)), g["list"]);
    W3L_SAME(summaries_json(list_conclusions(*core, "ws", 1)), g["listLimited"]);
    W3L_SAME(opt_detail(get_conclusion_detail(*core, "ws", "g1")), g["detail"]);
    FakeAttribution attribution;
    W3L_SAME(opt_detail(get_conclusion_detail(*core, "ws", "g1", &attribution)), g["detailAttributed"]);
    W3L_SAME(opt_detail(get_conclusion_detail(*core, "ws", "g2")), g["detailG2"]);
    W3L_SAME(opt_detail(get_conclusion_detail(*core, "ws", "g4")), g["detailNoSession"]);
    W3L_SAME(opt_detail(get_conclusion_detail(*core, "ws", "nope")), g["missing"]);
    CHECK_EQ(conclusion_speaker_label(std::nullopt), std::string("参加者"));
    CHECK_EQ(conclusion_speaker_label(std::string("")), std::string("参加者"));
    CHECK(str::starts_with(conclusion_speaker_label(std::string("ext:x:y")), "論者#"));

    // 学習キャッシュ: KG から焼き、読み取り専用で読み戻す。
    const std::string cache_path = dir.path + "/sub/learning-cache.sqlite";
    CHECK(!learning_cache_exists(cache_path));
    const auto built = build_learning_cache(*core, "ws", cache_path, 1234);
    CHECK_EQ(built.builtAt, 1234);
    CHECK_EQ(built.conclusions, 3);  // g2 はセッションが 2 つあり JOIN で 2 行になる (TS と同じ)
    CHECK_EQ(built.details, 3);
    CHECK_EQ(built.layers, 3);
    CHECK(learning_cache_exists(cache_path));
    auto reader = open_learning_cache_reader(cache_path);
    W3L_SAME(reader->conclusions(), (Json{{"conclusions", g["list"]}}));
    W3L_SAME(*reader->conclusion("g1"), g["detail"]);
    CHECK(!reader->conclusion("g3").has_value());
    CHECK(reader->layer("knowledge").has_value());
    CHECK(!reader->gap("none").has_value());
    W3L_SAME(reader->gap_summary(), (Json{{"games", Json::array()}}));
    CHECK_EQ(*reader->built_at(), 1234);
    reader->close();

    // 壊れた payload は無いものとして扱う (既定値に戻る)
    {
        auto raw = sql::Database::open(cache_path);
        raw->run("UPDATE cache SET payload = '{broken' WHERE key = 'conclusions'");
        raw->run("UPDATE cache SET payload = '[1,2]' WHERE key = '__meta__'");
        raw->close();
    }
    auto reader2 = open_learning_cache_reader(cache_path);
    W3L_SAME(reader2->conclusions(), (Json{{"conclusions", Json::array()}}));
    CHECK(!reader2->built_at().has_value());
    reader2->close();
    CHECK_THROWS(open_learning_cache_reader(dir.path + "/missing.sqlite"));

    // gap キャッシュ: games md の無いディレクトリは 0 件 (summary だけ焼く)
    CHECK_EQ(build_gap_cache(cache_path, dir.path + "/no-games", dir.path + "/kg.sqlite", 5).games, 0);
    auto reader3 = open_learning_cache_reader(cache_path);
    W3L_SAME(reader3->gap_summary(), (Json{{"games", Json::array()}}));
    reader3->close();
    core->close();
}

int main() { return di::test::run_all(); }
