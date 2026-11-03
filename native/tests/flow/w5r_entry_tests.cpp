// w5-R: src/flow/entry-discord.ts の移植の検証。
// tests/flow/entrypoints.test.ts の parseForumEntry / handleForumFlowPost と、golden (Node の入出力)・境界。
#include "check.hpp"
#include "w5r_support.hpp"

#include <chrono>

#include "flow/entry_discord.hpp"

using namespace di;
using namespace di::flow;

namespace {

std::optional<ForumTagMapping> mapping_from(const Json& j) {
    if (!j.is_object()) return std::nullopt;
    ForumTagMapping m;
    if (j.contains("flowKindTagNames")) {
        std::vector<std::pair<FlowKind, std::vector<std::string>>> v;
        for (auto it = j["flowKindTagNames"].begin(); it != j["flowKindTagNames"].end(); ++it)
            v.emplace_back(it.key(), it.value().get<std::vector<std::string>>());
        m.flowKindTagNames = v;
    }
    if (j.contains("flowTagNames")) m.flowTagNames = j["flowTagNames"].get<std::vector<std::string>>();
    return m;
}

DispatchDeps fake_deps(std::vector<std::string>& calls) {
    DispatchDeps d;
    d.llm = std::make_shared<w5r::FakeLLM>();
    auto drv = [&calls](const char* name) {
        return [&calls, name](persona_engine::LLMClient&, const std::string&, const std::vector<FlowTag>&, const FlowDirectorDeps&) {
            calls.push_back(name);
            FlowDirectorResult r;
            r.sessionId = "s";
            r.paperId = "p";
            r.rounds = 1;
            return r;
        };
    };
    d.drivers.discussion = drv("discussion");
    d.drivers.improvement = drv("improvement");
    return d;
}

} // namespace

TEST_CASE("golden: parseForumEntry が Node と一致") {
    const Json fx = w5r::load_fixture("entry.json");
    for (const auto& c : fx["forum"]) {
        const auto out = parse_forum_entry(c["names"].get<std::vector<std::string>>(), mapping_from(c.value("mapping", Json())));
        W5R_CHECK_JSON(to_json(out), c["out"], c["names"].dump());
    }
}

TEST_CASE("golden: parseRoundsTurns / parseOpponents が Node と一致") {
    const Json fx = w5r::load_fixture("entry.json");
    for (const auto& c : fx["roundsTurns"]) W5R_CHECK_JSON(to_json(parse_rounds_turns(c["text"].get<std::string>())), c["out"], c["text"].dump());
    for (const auto& c : fx["opponents"]) {
        const auto out = parse_opponents(c["text"].get<std::string>());
        W5R_CHECK_JSON(out ? Json(*out) : Json(nullptr), c["out"], c["text"].dump());
    }
}

TEST_CASE("entrypoints: parseForumEntry の観点 (TS テストと同じ)") {
    auto e = parse_forum_entry({"改善", "機密"});
    CHECK_EQ(e.flow.value_or(""), std::string("improvement"));
    CHECK_EQ(e.tags, std::vector<std::string>{"機密"});
    e = parse_forum_entry({"壁打ち", "運用"});
    CHECK(!e.flow);  // 廃止した壁打ちは起動しない
    CHECK_EQ(e.tags, std::vector<std::string>{"運用"});
    ForumTagMapping m;
    m.flowKindTagNames = std::vector<std::pair<FlowKind, std::vector<std::string>>>{{"sparring", {"旧相談"}}};
    CHECK(!parse_forum_entry({"旧相談"}, m).flow);
    CHECK_EQ(parse_forum_entry({"議論"}).flow.value_or(""), std::string("discussion"));
    CHECK(!parse_forum_entry({"面白さ"}).flow);
    CHECK(!parse_forum_entry({}).flow);
}

TEST_CASE("entrypoints: handleForumFlowPost は flow 必須 / 適用タグから dispatch") {
    std::vector<std::string> calls;
    const auto deps = fake_deps(calls);
    const auto rejected = handle_forum_flow_post({"テーマ", {"面白さ"}, std::nullopt}, deps);
    CHECK(!rejected.ok);
    CHECK_EQ(rejected.reason, std::string("flow-required"));
    CHECK_EQ(rejected.message, std::string(FLOW_REQUIRED_MESSAGE));
    const auto accepted = handle_forum_flow_post({"テーマ", {"改善", "内部"}, std::string("discord:g/t")}, deps);
    CHECK(accepted.ok);
    CHECK(accepted.dispatch.has_value());
    CHECK_EQ(accepted.dispatch->kind, std::string("improvement"));
    CHECK_EQ(calls, std::vector<std::string>{"improvement"});
}

TEST_CASE("境界: 巨大・壊れた入力でも線形に読む") {
    // 1MiB の空白 + キーワードの繰り返し (正規表現の後戻りに相当する走査が二乗にならない)
    std::string big;
    for (int i = 0; i < 40000; ++i) big += "rounds round ラウンド 相手 opponent ";
    const auto t0 = std::chrono::steady_clock::now();
    const auto rt = parse_rounds_turns(big);
    const auto op = parse_opponents(big);
    std::string spaces(1u << 20, ' ');
    const auto rt2 = parse_rounds_turns("ラウンド" + spaces + ":" + spaces + "5");
    const auto op2 = parse_opponents("相手:" + spaces);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(!rt.rounds);
    CHECK(!op);
    CHECK_EQ(rt2.rounds.value_or(0), 5.0);
    CHECK(!op2);  // 空白だけ → 名前なし
    CHECK(elapsed < std::chrono::seconds(10));
    // 壊れた UTF-8 でも落ちない
    const std::string broken = std::string("ラウンド:3 \xff\xfe 相手:\xc3") + "名";
    CHECK_EQ(parse_rounds_turns(broken).rounds.value_or(0), 3.0);
    CHECK(parse_opponents(broken).has_value());
    // 上限ちょうど・上限 + 1 は無いが、桁あふれ (Infinity) は次のパターンへ
    CHECK_EQ(parse_rounds_turns("ラウンド:" + std::string(400, '9') + " rounds=2").rounds.value_or(0), 2.0);
    CHECK(!parse_rounds_turns("ラウンド:" + std::string(400, '9')).rounds);
    // 多数のタグ
    std::vector<std::string> tags(100000, "x");
    tags.push_back("議論");
    CHECK_EQ(parse_forum_entry(tags).flow.value_or(""), std::string("discussion"));
}

int main() { return di::test::run_all(); }
