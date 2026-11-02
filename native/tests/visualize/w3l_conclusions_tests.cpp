// w3-L: visualize の結論一覧・詳細・キャッシュ・md エクスポート・学習キャッシュのテスト。
//   - golden: tests/golden/w3-L-flow.gen.ts が記録した Node の出力と一致すること (md は 1 バイトも違わない)。
//   - tests/conclusion-cache/conclusion-cache.test.ts, tests/visualize/flow-conclusions.test.ts,
//     tests/visualize/conclusion-markdown.test.ts の観点。
//   - 境界: DB の JSON 列 (上限ちょうど・上限 + 1・壊れた・巨大)、Date の範囲外。
#include "check.hpp"
#include "flow/w3l_support.hpp"

#include <filesystem>

#include "base/sqlite.hpp"
#include "base/strings.hpp"
#include "core/index.hpp"
#include "flow/db/migrations.hpp"
#include "lib/front_matter/front_matter.hpp"
#include "visualize/conclusion_cache.hpp"
#include "visualize/conclusion_markdown.hpp"
#include "visualize/conclusions.hpp"
#include "visualize/flow_conclusions.hpp"
#include "visualize/learning_cache.hpp"

using namespace di;
using namespace di::visualize;
using w3l::Json;

namespace {

struct TempDir {
    TempDir() {
        path = (std::filesystem::temp_directory_path() / ("w3l-vis-" + crypto::uuid_v4())).string();
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string path;
};

Json summaries_json(const std::vector<ConclusionSummary>& list) {
    Json a = Json::array();
    for (const auto& s : list) a.push_back(to_json(s));
    return a;
}

Json opt_detail(const std::optional<ConclusionDetail>& d) { return d ? to_json(*d) : Json(nullptr); }

std::shared_ptr<sql::Database> flow_db(const std::string& path) {
    auto db = sql::Database::open(path);
    flow::db::apply_flow_migrations(*db);
    return db;
}

ConclusionSummary summary(const std::string& gap, const std::string& title, std::int64_t updated) {
    ConclusionSummary s;
    s.gapId = gap;
    s.kind = "flow";
    s.sessionId = gap.substr(5);
    s.title = title;
    s.conclusion = "達成感";
    s.utteranceCount = 18;
    s.aufhebungCount = 3;
    s.updatedAt = updated;
    return s;
}

class FakeAttribution : public crawler::sources::AttributionStore {
public:
    void set(const crawler::sources::AttributionInput&) override {}
    std::optional<crawler::sources::Attribution> get(const std::string& id) override {
        crawler::sources::Attribution a;
        a.utteranceId = id;
        if (id == "x2") {
            a.source = "youtube";
            a.sourceUrl = "https://youtu.be/abc";
            return a;
        }
        if (id == "x6") {
            a.source = "steam";
            a.sourceUrl = "";
            return a;
        }
        return std::nullopt;
    }
    std::vector<crawler::sources::Attribution> list_by_source(const std::string&, const std::optional<std::string>&, double) override { return {}; }
    std::vector<crawler::sources::SourceCount> counts_by_source() override { return {}; }
    void close() override {}
};

} // namespace

TEST_CASE("golden: safe_slug / render_conclusion_markdown (Node と 1 バイトも違わない)") {
    for (const auto& c : w3l::golden()["slugs"])
        CHECK_EQ(safe_slug(c["title"].get<std::string>(), c["fallback"].get<std::string>()), c["slug"].get<std::string>());
    for (const auto& c : w3l::golden()["markdown"]) {
        const auto detail = conclusion_detail_from_json(c["detail"]);
        W3L_SAME(to_json(detail), c["detail"]);  // JSON の往復でも値が変わらない
        CHECK_EQ(render_conclusion_markdown(detail), c["md"].get<std::string>());
    }
}

TEST_CASE("conclusion-markdown.test.ts の観点 (front matter / 節 / 出所 / paper なし / 未生成)") {
    const auto detail = conclusion_detail_from_json(w3l::golden()["markdown"][0]["detail"]);
    const auto parsed = lib::front_matter::parse(render_conclusion_markdown(detail));
    CHECK(parsed.has_value());
    CHECK_EQ(parsed->data["type"].get<std::string>(), std::string("discussion-conclusion"));
    CHECK_EQ(parsed->data["kind"].get<std::string>(), std::string("flow"));
    CHECK_EQ(parsed->data["id"].get<std::string>(), std::string("flow:sess-1"));
    CHECK(parsed->data["concluded"].get<bool>());
    CHECK(str::contains(parsed->content, "## 議論ログ (3 発話)"));
    CHECK(str::contains(parsed->content, "([youtube-livechat](https://youtu.be/x))"));
    CHECK(str::contains(parsed->content, "**ビルド選択**: 周回ごとに構成を変える → 期待感情: 発見"));
    auto no_paper = detail;
    no_paper.paper.reset();
    CHECK(!str::contains(render_conclusion_markdown(no_paper), "## ディスカッションペーパー"));
    auto no_conc = detail;
    no_conc.conclusion.reset();
    const auto p2 = lib::front_matter::parse(render_conclusion_markdown(no_conc));
    CHECK(!p2->data["concluded"].get<bool>());
    CHECK(str::contains(p2->content, "(まとめ未生成)"));
    // Date の範囲: ちょうど ±8.64e15 は書ける、+1 は RangeError と同じく例外。
    auto edge = detail;
    edge.updatedAt = 8'640'000'000'000'000LL;
    (void)render_conclusion_markdown(edge);
    edge.updatedAt += 1;
    CHECK_THROWS(render_conclusion_markdown(edge));
    // 巨大なタイトル: スラグは 60 UTF-16 単位まで
    CHECK_EQ(str::js_length(safe_slug(std::string(1u << 20, 'a'), "x")), 60u);
}

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
