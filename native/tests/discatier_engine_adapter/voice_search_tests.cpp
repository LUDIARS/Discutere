// w3-N: 外部の声の検索 (voice-search)・adapter の外部の声 (仮名化・出所)・external-voices の配線・機密タグの遮断。
// tests/persona-engine/external-voices.test.ts, external-voices-lookup.test.ts の移植 + golden (Node と順位が一致) + 境界。
#include <atomic>
#include <chrono>
#include <thread>

#include "check.hpp"
#include "w3n_support.hpp"

#include "base/strings.hpp"
#include "base/time.hpp"
#include "core/vectors/embedder.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "crawler/sources/persona.hpp"
#include "discatier_engine_adapter/index.hpp"
#include "discatier_engine_adapter/voice_search.hpp"
#include "flow/external_voices.hpp"
#include "flow/tags.hpp"

using namespace di;
using namespace di::discatier_engine_adapter;

namespace {

class StubEmbedder : public core::vectors::EmbeddingClient {
public:
    explicit StubEmbedder(std::vector<double> v) : v_(std::move(v)) {}
    std::vector<std::vector<double>> embed(const std::vector<std::string>& texts) override {
        calls += 1;
        return std::vector<std::vector<double>>(texts.size(), v_);
    }
    std::atomic<int> calls{0};

private:
    std::vector<double> v_;
};

std::vector<std::string> strings_of(const Json& arr) {
    std::vector<std::string> out;
    for (const auto& x : arr) out.push_back(x.get<std::string>());
    return out;
}

/** golden の DB を組む (attribution は KG ディレクトリの sidecar)。 */
std::unique_ptr<core::Core> golden_core(const w3n::TempDir& dir, const Json& fx) {
    auto core = core::create_core(dir.file("kg/discatier.kuzu"), dir.file("events.jsonl"));
    w3n::load_tables(*core, fx["tables"]);
    auto store = crawler::sources::open_attribution_store();
    for (const auto& a : fx["attributions"]) {
        crawler::sources::AttributionInput in;
        in.utteranceId = a["utteranceId"].get<std::string>();
        in.source = a["source"].get<std::string>();
        in.sourceUrl = a["sourceUrl"].get<std::string>();
        in.nativeId = a["nativeId"].get<std::string>();
        if (a["gameSlug"].is_string()) in.gameSlug = a["gameSlug"].get<std::string>();
        in.at = a["at"].get<double>();
        store->set(in);
    }
    store->close();
    return core;
}

core::repositories::UtteranceInput utt(const std::string& id, const std::string& speaker, const std::string& text, double posted) {
    core::repositories::UtteranceInput u;
    u.id = id;
    u.workspaceId = "knowledge";
    u.sessionId = "ext:s";
    u.speakerId = speaker;
    u.rawContent = text;
    u.postedAt = posted;
    return u;
}

} // namespace

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
