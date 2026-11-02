// w2-J: attribution-store (tests/crawler/external/attribution-store.test.ts) と summarize (summary.test.ts の summarizeItems)。
#include "check.hpp"
#include "core/test_util.hpp"

#include "base/strings.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "crawler/sources/summarize.hpp"
#include "persona_engine/llm/mock.hpp"

using namespace di;
using namespace di::crawler::sources;

TEST_CASE("attribution store: set/get・upsert・listBySource・countsBySource") {
    test::TempDir dir;
    auto store = open_attribution_store(dir.file("sub/.attribution.sqlite"));
    store->set({"ext:youtube:c1", "youtube", "https://youtu.be/abc?lc=c1", "c1", std::string("hollow-knight"), 1000});
    auto got = store->get("ext:youtube:c1");
    CHECK(got && got->source == "youtube" && got->sourceUrl == "https://youtu.be/abc?lc=c1" && got->gameSlug == std::string("hollow-knight"));
    CHECK(!store->get("missing"));
    store->set({"ext:youtube:c1", "youtube", "https://updated", "c1", std::string("hk"), 2000});
    CHECK_EQ(store->get("ext:youtube:c1")->sourceUrl, std::string("https://updated"));
    store->set({"ext:steam:r1", "steam", "https://s/r1", "r1", std::string("hk"), 3000});
    store->set({"ext:steam:r2", "steam", "https://s/r2", "r2", std::string("celeste"), 4000});
    store->set({"ext:steam:r3", "steam", "https://s/r3", "r3", std::nullopt, std::nullopt});
    const auto all = store->list_by_source("steam");
    CHECK(all.size() == 3 && all[1].utteranceId == "ext:steam:r2");
    const auto hk = store->list_by_source("steam", std::string("hk"));
    CHECK(hk.size() == 1 && hk[0].nativeId == "r1");
    CHECK_EQ(store->list_by_source("steam", std::nullopt, 1).size(), std::size_t{1});
    const auto counts = store->counts_by_source();
    CHECK_EQ(counts[0].source, std::string("steam"));
    CHECK_EQ(counts[0].count, 3.0);
    auto slugs = counts[0].gameSlugs;
    std::sort(slugs.begin(), slugs.end());
    CHECK(slugs == (std::vector<std::string>{"celeste", "hk"}));
    CHECK(counts[0].lastImportedAt.has_value());
    CHECK_EQ(json_stringify(to_json(*store->get("ext:steam:r1"))),
             json_stringify(Json{{"utteranceId", "ext:steam:r1"}, {"source", "steam"}, {"sourceUrl", "https://s/r1"}, {"nativeId", "r1"}, {"gameSlug", "hk"}, {"importedAt", 3000}}));
    store->close();
}

TEST_CASE("summarize_items: 長文だけ要約 (境界: minChars ちょうど/-1)") {
    struct Fake : Summarizer {
        int calls = 0;
        std::optional<std::string> summarize(const std::string& text) override {
            ++calls;
            return "要約(" + std::to_string(str::js_length(text)) + "字の記事)";
        }
    } fake;
    std::vector<ExternalUtterance> items(4);
    items[0].content = str::repeat("ワンダと巨像のレビュー本文。", 60);
    items[1].content = "短い感想";
    items[2].content = std::string(600, 'a');
    items[3].content = std::string(599, 'a');
    int progress = 0;
    const int n = summarize_items(items, fake, {600.0, [&](int d) { progress = d; }});
    CHECK_EQ(n, 2);
    CHECK_EQ(progress, 2);
    CHECK(items[0].summary && str::starts_with(*items[0].summary, "要約("));
    CHECK(!items[1].summary && !items[3].summary && items[2].summary);
    CHECK_EQ(summarize_items(items, fake), 0);  // summary 済みは飛ばす
}

TEST_CASE("LLM 要約器: system プロンプト・入力の上限・出力の切り詰め (golden)") {
    for (const auto& c : test::load_fixture("w2j/summarize.json")) {
        const std::string reply = c["reply"].get<std::string>();
        auto llm = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{
            [reply](const persona_engine::LLMInvokeArgs&) { return std::variant<std::string, persona_engine::LLMResult>(reply); }});
        auto s = create_llm_summarizer(llm, {std::nullopt, c["target"].get<double>(), 7.0});
        const auto out = s->summarize(c["input"].get<std::string>());
        CHECK_EQ(out ? Json(*out) : Json(nullptr), c["out"]);
        const auto last = llm->last_invocation();
        if (c["prompt"].is_null()) {
            CHECK(!last);
        } else {
            CHECK_EQ(last->prompt, c["prompt"].get<std::string>());
            CHECK_EQ(*last->system, c["system"].get<std::string>());
        }
    }
    auto failing = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [](const persona_engine::LLMInvokeArgs&) { return std::variant<std::string, persona_engine::LLMResult>(persona_engine::LLMResult::failure("x")); }});
    CHECK(!create_llm_summarizer(failing)->summarize("本文"));
}

DI_TEST_MAIN()
