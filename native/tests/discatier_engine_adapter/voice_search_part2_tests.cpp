// voice_search のテスト (2/2)。準備は voice_search_support.hpp。
#include "voice_search_support.hpp"

using namespace voice_search_support;

TEST_CASE("external-voices-lookup.test.ts: 部分索引はキーワード順へ degrade、索引完成後は RRF") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, true);
    auto core = core::create_core(dir.file("kg/discatier.kuzu"), dir.file("events.jsonl"));
    core->repos.utterance.create(utt("top", "ext:test:keyword-top", "経験値とXPの両方が成長実感につながる", 3000));
    core->repos.utterance.create(utt("low1", "ext:test:embedded-1", "経験値だけでは単調に感じる", 2000));
    core->repos.utterance.create(utt("low2", "ext:test:embedded-2", "XPの表示方法を変えてほしい", 1000));
    core->register_embedding({"knowledge", "utterance", "low1", {1, 0}});
    core->register_embedding({"knowledge", "utterance", "low2", {0.9, 0.1}});
    auto embedder = std::make_shared<StubEmbedder>(std::vector<double>{1, 0});
    core::vectors::warm_query_embedding_sync(core->raw_ptr(), embedder, "golden-model", core::vectors::normalize_query_text({"経験値"}),
                                             [](const std::string&) {});
    auto rows = search_external_voice_rows(*core, "knowledge", {"経験値"}, 1);
    CHECK_EQ(rows.at(0).id, std::string("top"));
    core->register_embedding({"knowledge", "utterance", "top", {0, 1}});
    rows = search_external_voice_rows(*core, "knowledge", {"経験値"}, 1);
    CHECK_EQ(rows.at(0).id, std::string("low1"));
    core->close();
}

TEST_CASE("未命中のクエリは今回キーワード順で返し、裏で温めて次回からハイブリッド") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, true);
    auto stub = std::make_shared<StubEmbedder>(std::vector<double>{1, 0});
    set_voice_embedder_for_test(stub);
    auto core = core::create_core(dir.file("kg/discatier.kuzu"), dir.file("events.jsonl"));
    core->repos.utterance.create(utt("top", "ext:t:a", "経験値とXPの両方が成長実感につながる", 3000));
    core->repos.utterance.create(utt("low1", "ext:t:b", "経験値だけでは単調に感じる", 2000));
    core->repos.utterance.create(utt("low2", "ext:t:c", "XPの表示方法を変えてほしい", 1000));
    core->register_embedding({"knowledge", "utterance", "low2", {0.9, 0.1}});
    core->register_embedding({"knowledge", "utterance", "top", {0, 1}});
    core->register_embedding({"knowledge", "utterance", "low1", {1, 0}});
    auto rows = search_external_voice_rows(*core, "knowledge", {"経験値"}, 1);
    CHECK_EQ(rows.at(0).id, std::string("top"));
    const auto query = core::vectors::normalize_query_text({"経験値"});
    for (int i = 0; i < 200 && !core::vectors::get_cached_query_vector(core->raw(), "golden-model", query); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(core::vectors::get_cached_query_vector(core->raw(), "golden-model", query).has_value());
    CHECK_EQ(stub->calls.load(), 1);
    rows = search_external_voice_rows(*core, "knowledge", {"経験値"}, 1);
    CHECK_EQ(rows.at(0).id, std::string("low1"));
    // useEmbedding=false はキャッシュがあってもキーワード順。
    SearchOptions opt;
    opt.useEmbedding = false;
    CHECK_EQ(search_external_voice_rows(*core, "knowledge", {"経験値"}, 1, {}, opt).at(0).id, std::string("top"));
    set_voice_embedder_for_test(nullptr);
    core->close();
}

TEST_CASE("境界: LIKE 候補の上限 (2000 ちょうど / 2001)・limit・LIKE のメタ文字・巨大な本文・壊れた vector") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("kg/discatier.kuzu"), dir.file("events.jsonl"));
    core->raw().transaction([&] {
        for (int i = 0; i < KEYWORD_CAP + 1; ++i)
            core->repos.utterance.create(utt("c" + std::to_string(i), "ext:t:u", "キャップ試験 本文 " + std::to_string(i) + " 番の発話", i));
    });
    // 近似重複畳み前の候補は KEYWORD_CAP 件で打ち切られる (limit を大きくしても 2000 を超えない)。
    auto rows = search_external_voice_rows(*core, "knowledge", {"キャップ試験"}, 5000);
    CHECK(rows.size() <= static_cast<std::size_t>(KEYWORD_CAP));
    CHECK(!rows.empty());
    CHECK(search_external_voice_rows(*core, "knowledge", {"キャップ試験"}, 0).empty());
    CHECK(search_external_voice_rows(*core, "knowledge", {"キャップ試験"}, -1).empty());
    CHECK_EQ(search_external_voice_rows(*core, "knowledge", {"キャップ試験"}, 1).size(), std::size_t{1});

    // % _ \ はエスケープして文字として照合する (ワイルドカードにならない)。
    core->repos.utterance.create(utt("pct", "ext:t:p", "達成率 50%_ok です", 9e6));
    core->repos.utterance.create(utt("nopct", "ext:t:p", "達成率 50xyok です", 9e6 + 1));
    rows = search_external_voice_rows(*core, "knowledge", {"50%_ok"}, 10);
    CHECK_EQ(rows.size(), std::size_t{1});
    CHECK_EQ(rows[0].id, std::string("pct"));

    // 巨大な本文 (1MiB) も 200 UTF-16 で切って "…" を付ける。
    std::string huge = "巨大本文テスト";
    huge += std::string(1u << 20, 'a');
    core->repos.utterance.create(utt("huge", "ext:t:h", huge, 9e6 + 2));
    auto adapter = create_discatier_context_provider(*core);
    const auto voices = adapter->list_relevant_external_voices("knowledge", {"巨大本文テスト"}, 3);
    CHECK_EQ(voices.size(), std::size_t{1});
    CHECK_EQ(str::js_length(voices[0].content), EXTERNAL_VOICE_CONTENT_CAP + 1);
    CHECK(str::ends_with(voices[0].content, "…"));
    // ちょうど 200 文字は切らない。
    core->repos.utterance.create(utt("just", "ext:t:j", "ちょうど" + std::string(196, 'b'), 9e6 + 3));
    const auto just = adapter->list_relevant_external_voices("knowledge", {"ちょうど"}, 3);
    CHECK_EQ(just.at(0).content, "ちょうど" + std::string(196, 'b'));
    core->close();
}

TEST_CASE("make_list_external_voices: 出所だけ透過・毎回開いて閉じる・検索の例外は空、機密/内部タグで lookup ごと外す") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    {
        auto core = core::create_core(dir.file("kg/discatier.kuzu"), dir.file("events.jsonl"));
        core->repos.utterance.create(utt("v1", "ext:youtube:UCsecret", "ボス戦の声", 1));
        auto store = crawler::sources::open_attribution_store();
        store->set({"v1", "youtube", "https://example.invalid/v1", "n", std::nullopt, std::nullopt});
        store->close();
        core->close();
    }
    int opened = 0;
    auto lookup = flow::make_list_external_voices(
        [&] {
            opened += 1;
            return core::create_core(dir.file("kg/discatier.kuzu"), dir.file("events.jsonl"));
        },
        "knowledge");
    const auto voices = lookup({"ボス戦"}, 5);
    CHECK_EQ(voices.size(), std::size_t{1});
    CHECK_EQ(voices[0].source, std::string("youtube"));
    CHECK_EQ(voices[0].sourceUrl.value_or(""), std::string("https://example.invalid/v1"));
    CHECK(!str::contains(json_stringify(flow::to_json(voices[0])), "UCsecret"));
    CHECK_EQ(opened, 1);

    // 検索中の例外 (壊れた DB) は空。
    auto broken = flow::make_list_external_voices(
        [&] {
            auto core = core::create_core(dir.file("kg2/discatier.kuzu"), dir.file("events2.jsonl"));
            core->raw().exec("DROP TABLE utterances");
            return core;
        },
        "knowledge");
    CHECK(broken({"ボス戦"}, 5).empty());
    // open_core の例外は TS と同じく呼び出し側へ。
    auto failing = flow::make_list_external_voices([]() -> std::unique_ptr<core::Core> { throw std::runtime_error("cannot open"); }, "knowledge");
    CHECK_THROWS(failing({"ボス戦"}, 5));

    // 機密 / 内部: 蓄積済みの外部の声も参照させない (lookup ごと外す)。それ以外のタグは通す。
    for (const auto& tag : {"機密", "内部"}) {
        const auto gated = flow::gate_external_voices<flow::ListExternalVoicesFn>({"開発", tag}, lookup);
        CHECK(!gated.has_value());
    }
    const auto open_tags = flow::gate_external_voices<flow::ListExternalVoicesFn>({"開発", "運用", "未来予測"}, lookup);
    CHECK(open_tags.has_value());
    CHECK_EQ((*open_tags)({"ボス戦"}, 5).size(), std::size_t{1});
    CHECK(flow::gate_external_voices<flow::ListExternalVoicesFn>({}, lookup).has_value());
}

DI_TEST_MAIN()
