// w3-N: auto-discussion (投稿の分類 → designGap)。tests/discord-hook/auto-discussion.test.ts の移植 + golden + 境界。
#include "check.hpp"
#include "w3n_support.hpp"

#include "base/strings.hpp"
#include "base/time.hpp"
#include "discord_hook/auto_discussion.hpp"
#include "persona_engine/llm/mock.hpp"

using namespace di;
using namespace di::discord_hook;
namespace pe = di::persona_engine;

namespace {

/** 決まった応答 (か失敗) を返し、引数を記録する LLM。 */
class FixedLlm : public pe::LLMClient {
public:
    explicit FixedLlm(std::optional<std::string> text) : text_(std::move(text)) {}
    pe::LLMResult invoke(const pe::LLMInvokeArgs& args) override {
        last = args;
        calls += 1;
        return text_ ? pe::LLMResult::success(*text_) : pe::LLMResult::failure("mock failure");
    }
    std::optional<pe::LLMInvokeArgs> last;
    int calls = 0;

private:
    std::optional<std::string> text_;
};

std::optional<std::string> opt_str(const Json& v) { return v.is_string() ? std::optional<std::string>(v.get<std::string>()) : std::nullopt; }

DiscordAutoDiscussionInput input_of(const Json& j) {
    DiscordAutoDiscussionInput in;
    in.workspaceId = j["workspaceId"];
    in.guildId = j["guildId"];
    in.channelId = j["channelId"];
    in.sessionId = j["sessionId"];
    in.utteranceId = j["utteranceId"];
    in.authorId = j["authorId"];
    in.content = j["content"];
    if (j.contains("forumTitle")) in.forumTitle = j["forumTitle"].get<std::string>();
    if (j.contains("direction")) in.direction = j["direction"].get<std::string>();
    if (j.contains("forumTags")) in.forumTags = j["forumTags"].get<std::vector<std::string>>();
    return in;
}

} // namespace

TEST_CASE("golden: fallback 分類・方向性ディレクティブが Node と一致") {
    const Json fx = w3n::load_fixture("w3n/auto_discussion.json");
    for (const auto& c : fx["fallback"])
        CHECK_EQ(json_stringify(to_json(classify_discord_message_fallback(c["content"], opt_str(c["threadTitle"])))), json_stringify(c["result"]));
    for (const auto& d : fx["directives"]) CHECK_EQ(forum_direction_directive(d["direction"]), d["text"].get<std::string>());
}

TEST_CASE("golden: LLM 経由の分類 (JSON の切り出し・降格・正規化・失敗) とプロンプトが Node と一致") {
    const Json fx = w3n::load_fixture("w3n/auto_discussion.json");
    for (const auto& c : fx["llm"]) {
        auto llm = std::make_shared<FixedLlm>(c["llm"]["ok"].get<bool>() ? std::optional<std::string>(c["llm"]["text"].get<std::string>())
                                                                         : std::nullopt);
        const auto& expected = c["result"];
        try {
            const auto r = classify_discord_message(c["content"], llm, opt_str(c["threadTitle"]));
            CHECK(expected["ok"].get<bool>());
            CHECK_EQ(json_stringify(to_json(r)), json_stringify(expected["value"]));
        } catch (const std::runtime_error&) {
            CHECK(!expected["ok"].get<bool>());
        }
        const auto& inv = c["invocation"];
        CHECK(llm->last.has_value());
        CHECK_EQ(llm->last->prompt, inv["prompt"].get<std::string>());
        CHECK_EQ(llm->last->system.value_or(""), inv["system"].get<std::string>());
        CHECK_EQ(llm->last->maxTokens.value_or(0), inv["maxTokens"].get<int>());
    }
}

TEST_CASE("golden: start_auto_discussion の gap (タイトル・説明・gap_in・evidence) が Node と一致") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    const Json fx = w3n::load_fixture("w3n/auto_discussion.json");
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    for (const auto& c : fx["starts"]) {
        pe::LLMClientPtr llm;
        if (c["llmText"].is_string()) llm = std::make_shared<FixedLlm>(c["llmText"].get<std::string>());
        const auto r = start_auto_discussion_for_discord_message(*core, input_of(c["input"]), llm);
        CHECK_EQ(r.started, c["started"].get<bool>());
        CHECK_EQ(r.gapId.has_value(), c["hasGapId"].get<bool>());
        CHECK_EQ(json_stringify(to_json(r.classification)), json_stringify(c["classification"]));
        if (r.gapId) {
            auto row = core->raw().get(
                "SELECT title, description, status, gap_in, expected_affect, observed_affect, evidence_json FROM design_gaps WHERE id = ?", {*r.gapId});
            Json got = row->to_json();
            got["gap_in"] = str::replace_all(got["gap_in"].get<std::string>(), *r.gapId, "<gapId>");
            CHECK_EQ(json_stringify(got), json_stringify(c["gap"]));
        }
    }
    core->close();
}

TEST_CASE("auto-discussion.test.ts: fallback で開始・同じ発話で二重に立てない・LLM の gameTitle・降格・forumTitle・dedup 衝突しない") {
    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    DiscordAutoDiscussionInput in;
    in.workspaceId = "knowledge";
    in.guildId = "g1";
    in.channelId = "ch1";
    in.sessionId = "s1";
    in.utteranceId = "u-1";
    in.authorId = "u1";
    in.content = "ヴァンサバに必要な要素とは何か？";
    auto r = start_auto_discussion_for_discord_message(*core, in);
    CHECK(r.started && r.gapId);
    auto gap = core->raw().get("SELECT title, evidence_json FROM design_gaps WHERE id = ?", {*r.gapId});
    CHECK(str::contains(gap->str("title"), "Discord議題"));
    CHECK(str::contains(gap->str("evidence_json"), "u-1"));
    CHECK_EQ(core->raw().get("SELECT event_type FROM events WHERE event_type = 'DesignGapDetected' LIMIT 1")->str("event_type"),
             std::string("DesignGapDetected"));
    const auto n = core->raw().get("SELECT COUNT(*) AS n FROM design_gaps")->i64("n");
    CHECK(!start_auto_discussion_for_discord_message(*core, in).started);
    CHECK_EQ(core->raw().get("SELECT COUNT(*) AS n FROM design_gaps")->i64("n"), n);

    auto llm = std::make_shared<pe::MockLLMClient>(std::vector<pe::MockResponder>{[](const pe::LLMInvokeArgs&) -> std::variant<std::string, pe::LLMResult> {
        return std::string(R"({"action":"start_discussion","category":"opinion","gameTitle":"Vampire Survivors","genre":"ローグライト","title":"LLM分類議題","description":"LLMで分類した議題","reason":"mock"})");
    }});
    const auto c = classify_discord_message("Vampire Survivors はテンポが悪いと思う", llm);
    CHECK_EQ(llm->calls(), 1);
    CHECK_EQ(c.action, std::string("start_discussion"));
    CHECK_EQ(c.gameTitle.value_or(""), std::string("Vampire Survivors"));
    CHECK_EQ(c.genre.value_or(""), std::string("ローグライト"));

    auto vague = std::make_shared<FixedLlm>(std::string(R"({"action":"start_discussion","category":"opinion","gameTitle":""})"));
    CHECK_EQ(classify_discord_message("なんか最近のゲームつまらないよね", vague).action, std::string("record_only"));

    // 同カテゴリ + 同 affect の議論を 2 件立てても gap_in が一意 (UNIQUE 制約で落ちない)。
    auto a = in, b = in;
    a.utteranceId = "ua";
    a.channelId = "th-a";
    a.content = "このゲームのテンポは良いか悪いか議論したい";
    b.utteranceId = "ub";
    b.channelId = "th-b";
    b.content = a.content;
    const auto ra = start_auto_discussion_for_discord_message(*core, a);
    const auto rb = start_auto_discussion_for_discord_message(*core, b);
    CHECK(ra.started && rb.started);
    CHECK(*ra.gapId != *rb.gapId);
    CHECK(core->raw().get("SELECT gap_in FROM design_gaps WHERE id = ?", {*ra.gapId})->str("gap_in") !=
          core->raw().get("SELECT gap_in FROM design_gaps WHERE id = ?", {*rb.gapId})->str("gap_in"));
    core->close();
}

TEST_CASE("境界: LLM 応答の大きさ (上限ちょうど / +1)・深さ・壊れた JSON・巨大な evidence_json") {
    // 上限ちょうどは読む、+1 は読まない (fallback)。
    const std::string head = R"({"action":"record_only","category":"opinion","title":"T","pad":")";
    const std::string tail = "\"}";
    std::string exact = head + std::string(kMaxClassificationTextBytes - head.size() - tail.size(), 'x') + tail;
    CHECK_EQ(exact.size(), kMaxClassificationTextBytes);
    CHECK(parse_classification_json(exact).has_value());
    CHECK(!parse_classification_json(exact + " ").has_value());
    CHECK(!parse_classification_json(std::string(64u << 20, '{')).has_value());
    std::string deep = "{\"a\":" + std::string(40, '[') + std::string(40, ']') + "}";
    CHECK(!parse_classification_json(deep).has_value());
    CHECK(!parse_classification_json("{ broken").has_value());
    CHECK(!parse_classification_json("}{").has_value());
    CHECK(parse_classification_json("prefix {\"a\":1} suffix").has_value());

    // 上限を超えた応答は読まずに fallback の判定 (ゲーム名が無いので LLM 経路の降格で record_only)。
    auto huge = std::make_shared<FixedLlm>(exact + " ");
    const auto c = classify_discord_message("ヴァンサバに必要な要素とは何か？", huge);
    CHECK_EQ(c.action, std::string("record_only"));
    CHECK_EQ(c.category, std::string("game_design_question"));
    // スレッドタイトルがあれば降格しない。
    CHECK_EQ(classify_discord_message("ヴァンサバに必要な要素とは何か？", huge, std::string("スレ")).action, std::string("start_discussion"));

    w3n::TempDir dir;
    w3n::TestConfig cfg(dir, false);
    auto core = core::create_core(dir.file("db.kuzu"), dir.file("events.jsonl"));
    core::repositories::DesignGapInput g;
    g.workspaceId = "knowledge";
    g.title = "big";
    const auto gid = core->repos.designGap.create(g);
    // evidence_json の上限 + 1 は読まない (同じ発話でも「既存なし」)、壊れた JSON も飛ばす。
    std::string big = R"({"utteranceIds":["dup"],"pad":")" + std::string(kEvidenceJsonLimits.max_bytes, 'p') + "\"}";
    core->raw().run("UPDATE design_gaps SET evidence_json = ? WHERE id = ?", {big, gid});
    const auto g2 = core->repos.designGap.create(g);
    core->raw().run("UPDATE design_gaps SET evidence_json = '{broken' WHERE id = ?", {g2});
    DiscordAutoDiscussionInput in;
    in.workspaceId = "knowledge";
    in.utteranceId = "dup";
    in.content = "天気";
    const auto r = start_auto_discussion_for_discord_message(*core, in);
    CHECK(!r.gapId);
    core->close();
}

DI_TEST_MAIN()
