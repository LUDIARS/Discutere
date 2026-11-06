// w3l_conclusions のテスト (2/3)。準備は w3l_conclusions_support.hpp。
#include "w3l_conclusions_support.hpp"

using namespace w3l_conclusions_support;

TEST_CASE("golden: flow-conclusions の一覧・詳細・id とキャッシュの自己修復") {
    TempDir dir;
    auto db = flow_db(dir.path + "/fc.db");
    const Json& g = w3l::golden()["flowConclusions"];
    for (const auto& s : g["seedSql"]) db->exec(s.get<std::string>());
    W3L_SAME(summaries_json(list_flow_conclusions(*db, 100)), g["list"]);
    W3L_SAME(summaries_json(list_flow_conclusions(*db, 2)), g["listLimited"]);
    for (const auto& [sid, expected] : g["details"].items()) W3L_SAME(opt_detail(get_flow_conclusion_detail(*db, sid)), expected);
    for (const auto& c : g["ids"]) {
        CHECK_EQ(is_flow_conclusion_id(c["g"].get<std::string>()), c["is"].get<bool>());
        CHECK_EQ(flow_session_id_from_gap_id(c["g"].get<std::string>()), c["sid"].get<std::string>());
    }
    ensure_conclusion_cache_fresh(*db);
    W3L_SAME(summaries_json(list_cached_conclusions(*db, 100)), g["cacheAfterFresh"]);
    write_through_flow_conclusion(*db, {"sess-1", "新タイトル", std::nullopt, 9, 4, 5000});
    ConclusionSummary gx;
    gx.gapId = "gap-x";
    gx.kind = "gap";
    gx.sessionId = "s";
    gx.title = "t";
    gx.conclusion = "c";
    gx.utteranceCount = 3;
    gx.updatedAt = 4000;
    UpsertOptions uo;
    uo.materialCount = 12;
    uo.now = 1;
    upsert_conclusion_cache(*db, gx, uo);
    recompute_material_counts(
        *db,
        [](const ConclusionSummary& s) {
            if (s.gapId == "flow:sess-2") return std::numeric_limits<double>::quiet_NaN();
            if (s.gapId == "flow:sess-3") return -5.0;
            return static_cast<double>(s.utteranceCount * 10);
        },
        true);
    W3L_SAME(summaries_json(list_cached_conclusions(*db, 100)), g["cacheFinal"]);
    CHECK_EQ(conclusion_cache_count(*db), g["cacheCount"].get<std::int64_t>());
}

TEST_CASE("conclusion-cache.test.ts: upsert / list / write-through の温存 / recompute の失敗 skip / 自己修復の冪等") {
    TempDir dir;
    auto db = flow_db(dir.path + "/cc.db");
    CHECK_EQ(conclusion_cache_count(*db), 0);
    UpsertOptions a;
    a.discussionVolume = 18;
    a.materialCount = 42;
    upsert_conclusion_cache(*db, summary("flow:a", "A", 100), a);
    UpsertOptions b;
    b.discussionVolume = 24;
    b.materialCount = 7;
    upsert_conclusion_cache(*db, summary("flow:b", "B", 200), b);
    auto list = list_cached_conclusions(*db, 100);
    CHECK_EQ(list.size(), 2u);
    CHECK_EQ(list[0].gapId, std::string("flow:b"));
    CHECK_EQ(*list[0].discussionVolume, 24);
    CHECK_EQ(*list[1].materialCount, 42);
    CHECK_EQ(list_cached_conclusions(*db, 1).size(), 1u);
    CHECK_EQ(list_cached_conclusions(*db, 0).size(), 0u);

    UpsertOptions m;
    m.discussionVolume = 10;
    m.materialCount = 50;
    upsert_conclusion_cache(*db, summary("flow:s1", "X", 100), m);
    write_through_flow_conclusion(*db, {"s1", "「エルデンリング」は何が面白いのか", std::string("達成感と探索"), 30, 5, 300});
    const auto rows = list_cached_conclusions(*db, 10);
    const auto row = std::find_if(rows.begin(), rows.end(), [](const auto& r) { return r.gapId == "flow:s1"; });
    CHECK_EQ(*row->discussionVolume, 30);
    CHECK_EQ(*row->materialCount, 50);
    CHECK_EQ(*row->conclusion, std::string("達成感と探索"));
    CHECK_EQ(row->updatedAt, 300);

    upsert_conclusion_cache(*db, summary("flow:z", "壊れ", 3));
    const auto n = recompute_material_counts(*db, [](const ConclusionSummary& s) -> double {
        if (s.gapId == "flow:z") throw std::runtime_error("boom");
        return 1;
    });
    CHECK_EQ(n, 4);
    const auto after = list_cached_conclusions(*db, 10);
    for (const auto& r : after) CHECK_EQ(*r.materialCount, r.gapId == "flow:z" ? -1 : 1);

    db->exec("INSERT INTO discussion_paper (id, flow, session_id, theme, created_at, updated_at) VALUES ('p1','discussion','f1','「テトリス」',1,1)");
    db->exec("INSERT INTO flow_conclusion (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at) VALUES ('f1','p1','【収束】普遍性','[]','[]',1,9)");
    // 件数がすでに flow_conclusion 以上なら何もしない (COUNT 2 回で戻る)。
    ensure_conclusion_cache_fresh(*db);
    CHECK_EQ(conclusion_cache_count(*db), 4);
    db->exec("DELETE FROM conclusion_cache");
    ensure_conclusion_cache_fresh(*db);
    CHECK_EQ(conclusion_cache_count(*db), 1);
    ensure_conclusion_cache_fresh(*db);
    CHECK_EQ(conclusion_cache_count(*db), 1);
    CHECK_EQ(*list_cached_conclusions(*db, 1)[0].materialCount, -1);
}

int main() { return di::test::run_all(); }
