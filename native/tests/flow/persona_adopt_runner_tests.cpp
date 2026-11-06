// w3-O: C1 実在ユーザ採用ランナー (src/flow/persona-adopt-runner.ts)。TS にテストが無いので挙動を確かめる:
//   - ext: の発話だけを話者アンカーで集約 (出現順)・source は attribution 優先で無ければ speaker_id から
//   - weight = 1 + いいね (positive reaction の intensity 合計)・gameSlug は attribution から
//   - sourceFilter・dry (書き込まない) と却下理由の集計・utterances は絞り込み前の件数
#include "w3o/w3o_support.hpp"

#include <map>

#include "base/env.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "flow/db/connection.hpp"
#include "flow/persona_adopt_runner.hpp"
#include "flow/persona_pool.hpp"

using namespace di;
using namespace di::flow;

namespace {

class FakeAttribution : public crawler::sources::AttributionStore {
public:
    std::map<std::string, crawler::sources::Attribution> rows;
    void set(const crawler::sources::AttributionInput&) override {}
    std::optional<crawler::sources::Attribution> get(const std::string& id) override {
        auto it = rows.find(id);
        if (it == rows.end()) return std::nullopt;
        return it->second;
    }
    std::vector<crawler::sources::Attribution> list_by_source(const std::string&, const std::optional<std::string>&, double) override { return {}; }
    std::vector<crawler::sources::SourceCount> counts_by_source() override { return {}; }
    void close() override {}
};

void seed(sql::Database& db) {
    db.exec(R"(CREATE TABLE utterances (id TEXT PRIMARY KEY, workspace_id TEXT, speaker_id TEXT, raw_content TEXT);
               CREATE TABLE reactions (id TEXT PRIMARY KEY, workspace_id TEXT, utterance_id TEXT, reaction_type TEXT, intensity REAL);)");
    db.run("INSERT INTO utterances VALUES ('u1', 'knowledge', 'ext:youtube:A', '良い')");
    db.run("INSERT INTO utterances VALUES ('u2', 'knowledge', 'ext:steam:B', '悪い')");
    db.run("INSERT INTO utterances VALUES ('u3', 'knowledge', 'ext:youtube:A', '普通')");
    db.run("INSERT INTO utterances VALUES ('u4', 'knowledge', 'human', '人間')");
    db.run("INSERT INTO utterances VALUES ('u5', 'other', 'ext:youtube:C', '別 workspace')");
    db.run("INSERT INTO reactions VALUES ('r1', 'knowledge', 'u1', 'positive', 2)");
    db.run("INSERT INTO reactions VALUES ('r2', 'knowledge', 'u1', 'positive', 1.5)");
    db.run("INSERT INTO reactions VALUES ('r3', 'knowledge', 'u3', 'negative', 9)");
}

} // namespace

TEST_CASE("collect_speaker_opinions: 集約・weight・source・gameSlug・sourceFilter") {
    auto db = sql::Database::open(":memory:");
    seed(*db);
    FakeAttribution attr;
    crawler::sources::Attribution a;
    a.utteranceId = "u2";
    a.source = "steam-reviews";
    a.gameSlug = "alpha";
    attr.rows["u2"] = a;
    double n = 0;
    const auto speakers = collect_speaker_opinions(*db, attr, "knowledge", std::nullopt, &n);
    CHECK_EQ(n, 3.0);
    CHECK_EQ(speakers.size(), 2u);
    CHECK_EQ(speakers[0].speakerId, std::string("ext:youtube:A"));
    CHECK_EQ(speakers[0].source.value_or(""), std::string("youtube"));
    CHECK_EQ(speakers[0].opinions.size(), 2u);
    CHECK_EQ(*speakers[0].opinions[0].weight, 4.5);
    CHECK_EQ(*speakers[0].opinions[1].weight, 1.0);
    CHECK(!speakers[0].opinions[0].gameSlug);
    CHECK_EQ(speakers[1].source.value_or(""), std::string("steam-reviews"));
    CHECK_EQ(speakers[1].opinions[0].gameSlug.value_or(""), std::string("alpha"));

    const auto filtered = collect_speaker_opinions(*db, attr, "knowledge", std::string("steam-reviews"), &n);
    CHECK_EQ(filtered.size(), 1u);
    CHECK_EQ(n, 3.0);  // utterances は絞り込み前の件数 (TS の rows.length)
}

TEST_CASE("adopt_from_speakers: dry は書き込まず候補数と却下理由を返す") {
    w3o::TempDir dir;
    env::set_for_test("DATABASE_PATH", std::string_view(dir.path + "/flow.db"));
    env::set_for_test("DISCUTERE_CONFIG", std::string_view(dir.path + "/none.json"));
    config::reset_config_for_test();
    db::reset_flow_db_for_test();
    std::vector<SpeakerOpinions> speakers{{"ext:youtube:A", std::string("youtube"), {{"良い", std::nullopt, 1.0}}},
                                          {"ext:youtube:B", std::string("youtube"), {{"悪い", std::nullopt, 1.0}}}};
    AdoptRunOptions opts;
    opts.dry = true;
    const auto s = adopt_from_speakers(speakers, 7, opts);
    CHECK_EQ(s.speakers, 2.0);
    CHECK_EQ(s.utterances, 7.0);
    CHECK_EQ(s.adopted, 0.0);
    CHECK_EQ(s.rejected, 2.0);
    W3O_SAME(to_json(s), (Json{{"speakers", 2}, {"utterances", 7}, {"adopted", 0}, {"rejected", 2}, {"rejectedByReason", Json{{"too-few", 2}}}}));
    PoolPersonaFilter f;
    f.origin = "adopted";
    CHECK(list_pool_personas(f).empty());
    opts.dry = false;
    CHECK_EQ(adopt_from_speakers(speakers, 7, opts).rejected, 2.0);
    db::reset_flow_db_for_test();
}

TEST_CASE("run_adopt_from_kg: active KG (一時) を開いて集約する") {
    w3o::TempDir dir;
    const auto cwd = std::filesystem::current_path();
    std::filesystem::current_path(dir.path);
    env::set_for_test("DATABASE_PATH", std::string_view(dir.path + "/flow.db"));
    env::set_for_test("DISCUTERE_CONFIG", std::string_view(dir.path + "/none.json"));
    env::set_for_test("DISCATIER_KUZU_PATH", std::string_view(dir.path + "/kg/kg.sqlite"));
    env::set_for_test("DISCATIER_WORKSPACE", std::nullopt);
    config::reset_config_for_test();
    db::reset_flow_db_for_test();
    {
        auto c = core::create_core(dir.path + "/kg/kg.sqlite");
        c->raw().run(
            "INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('x1', 'knowledge', 's', 'ext:steam:Z', 'a', 1, 1, 1)");
        c->close();
    }
    AdoptRunOptions opts;
    opts.dry = true;
    const auto s = run_adopt_from_kg(opts);
    CHECK_EQ(s.speakers, 1.0);
    CHECK_EQ(s.utterances, 1.0);
    db::reset_flow_db_for_test();
    config::reset_config_for_test();
    std::filesystem::current_path(cwd);
}

DI_TEST_MAIN()
