// voice_search のテスト (1/2)。準備は voice_search_support.hpp。
#include "voice_search_support.hpp"

using namespace voice_search_support;

TEST_CASE("golden: search_external_voice_rows の順位とスコアが Node と一致 (キーワード / RRF / 部分索引 / 重複畳み / 除外)") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir);
    auto stub = std::make_shared<StubEmbedder>(std::vector<double>{1, 0});
    set_voice_embedder_for_test(stub);
    const Json fx = w3n::load_fixture("w3n/voice_search.json");
    auto core = golden_core(dir, fx);
    const std::string ws = fx["workspaceId"];
    for (const auto& q : fx["queries"]) {
        SearchOptions opt;
        opt.useEmbedding = q["useEmbedding"].get<bool>();
        const auto rows = search_external_voice_rows(*core, ws, strings_of(q["terms"]), q["limit"].get<int>(), [](const std::string&) {}, opt);
        Json got = Json::array();
        for (const auto& r : rows) got.push_back(to_json(r));
        CHECK_EQ(json_stringify(got), json_stringify(q["rows"]));
    }
    // 事前に温めたクエリだけを使うので、埋め込みサーバ (スタブ) は呼ばれない。
    CHECK_EQ(stub->calls.load(), 0);
    set_voice_embedder_for_test(nullptr);
    core->close();
}

TEST_CASE("golden: adapter の外部の声は出所を開示し個人を仮名にする (Node と一致)") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir);
    set_voice_embedder_for_test(std::make_shared<StubEmbedder>(std::vector<double>{1, 0}));
    const Json fx = w3n::load_fixture("w3n/voice_search.json");
    auto core = golden_core(dir, fx);
    auto adapter = create_discatier_context_provider(*core);
    for (const auto& q : fx["adapter"]) {
        const auto voices = adapter->list_relevant_external_voices(fx["workspaceId"], strings_of(q["terms"]), q["limit"].get<int>());
        Json got = Json::array();
        for (const auto& v : voices) got.push_back(persona_engine::to_json(v));
        CHECK_EQ(json_stringify(got), json_stringify(q["voices"]));
        for (const auto& v : voices) {
            // §6: 公開 ID (speaker_id) を露出しない。
            CHECK(str::starts_with(v.speakerLabel, "論者#"));
            CHECK(!str::contains(v.speakerLabel, "UC"));
            CHECK(!str::contains(v.speakerLabel, "ext:"));
        }
    }
    set_voice_embedder_for_test(nullptr);
    core->close();
}

TEST_CASE("external-voices.test.ts: 関連語の外部発話だけ・出所開示・仮名・関連語なしは空") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("kg/discatier.kuzu"), dir.file("events.jsonl"));
    const double old = static_cast<double>(time::now_ms() - 5000);
    const auto hit = core->repos.utterance.create(utt("hit", "ext:youtube:UCabc", "このゲームはボス戦が理不尽すぎると思う", old));
    core->repos.utterance.create(utt("noise", "ext:youtube:UCxyz", "今日は良い天気ですね", old));
    core->repos.utterance.create(utt("persona", "persona:advocate", "ボス戦は歯ごたえがあって良い", old));
    {
        auto store = crawler::sources::open_attribution_store();
        store->set({hit, "youtube", "https://www.youtube.com/watch?v=vid1&lc=c1", "c1", std::string("demo-game"), std::nullopt});
        store->close();
    }
    auto adapter = create_discatier_context_provider(*core);
    const auto voices = adapter->list_relevant_external_voices("knowledge", {"ボス戦"}, 6);
    CHECK_EQ(voices.size(), std::size_t{1});
    CHECK_EQ(voices[0].id, hit);
    CHECK_EQ(voices[0].source, std::string("youtube"));
    CHECK(voices[0].sourceUrl && str::contains(*voices[0].sourceUrl, "youtube.com"));
    CHECK_EQ(voices[0].speakerLabel, crawler::sources::masked_persona_label("ext:youtube:UCabc"));
    CHECK(!str::contains(voices[0].speakerLabel, "UCabc"));
    CHECK(voices[0].score > 0);
    CHECK(adapter->list_relevant_external_voices("knowledge", {}, 6).empty());
    core->close();
}

TEST_CASE("external-voices-lookup.test.ts: 新着 310 件に埋もれた最古の材料も全件 LIKE で拾う") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("kg/discatier.kuzu"), dir.file("events.jsonl"));
    core->repos.utterance.create(utt("hit", "ext:niconico:userA", "ゼルダの新作は探索が神ゲーだと思う", 1000));
    core->raw().transaction([&] {
        for (int i = 0; i < 310; ++i)
            core->repos.utterance.create(utt("n" + std::to_string(i), "ext:niconico:userB", "無関係な雑談コメント番号 " + std::to_string(i), 10000 + i));
    });
    auto adapter = create_discatier_context_provider(*core);
    const auto voices = adapter->list_relevant_external_voices("knowledge", {"ゼルダの伝説は面白いか"}, 6);
    CHECK(!voices.empty());
    bool found = false;
    for (const auto& v : voices) {
        if (v.id == "hit" && str::contains(v.content, "ゼルダ")) found = true;
        CHECK(!str::contains(v.content, "無関係"));
    }
    CHECK(found);
    core->close();
}

DI_TEST_MAIN()
