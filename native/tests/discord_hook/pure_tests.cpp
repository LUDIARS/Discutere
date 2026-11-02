// discord-hook の純関数 (Node の出力 fixtures/discord_hook/*.json と比べる) と
// tests/discord-hook/{forum-flow-tags,humanize,paper-gap-ui,paper-review-ack}.test.ts, tests/feedback/extract.test.ts の観点。
#include "base/strings.hpp"
#include "check.hpp"
#include "discord_hook/diagnostic_reply.hpp"
#include "discord_hook/facilitator_directives.hpp"
#include "discord_hook/forum_flow_tags.hpp"
#include "discord_hook/game_feedback_channel.hpp"
#include "discord_hook/normalize.hpp"
#include "discord_hook/paper_gap_ui.hpp"
#include "discord_hook/paper_review_ack.hpp"
#include "discord_hook/poster.hpp"
#include "design_diagnostic/contracts.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::discord_hook;

TEST_CASE("builders match discord.js toJSON") {
    const Json f = w1d::load_fixture("discord_hook/builders.json");
    w1d::check_json_eq(build_flow_pick_menu("999"), f["flowPickMenu"], "flow pick");
    const auto rows = build_flow_settings_components("t1");
    w1d::check_json_eq(Json(rows), f["flowSettings"], "flow settings");
    w1d::check_json_eq(build_flow_settings_modal("t2"), f["flowSettingsModal"], "flow modal");
    std::size_t i = 0;
    for (const auto& t : paper_gap_types()) w1d::check_json_eq(build_paper_gap_modal(t.value, "ch9"), f["paperGapModals"][i++], t.value);
}

TEST_CASE("forum tag sets and custom ids") {
    CHECK_EQ(flow_kind_tag_names(), (std::vector<std::string>{"議論", "改善"}));
    CHECK_EQ(flow_aspect_tag_names(), (std::vector<std::string>{"機密", "内部", "運用", "開発", "未来予測", "代理"}));
    CHECK_EQ(all_flow_tag_names().size(), std::size_t(8));
    CHECK_EQ(*parse_flow_pick_custom_id("flow-pick:12345"), std::string("12345"));
    CHECK(!parse_flow_pick_custom_id("debate:cont:xx"));
    CHECK(!parse_flow_pick_custom_id("flow-pick:"));
    CHECK_EQ(*parse_flow_settings_custom_id("flow-settings:a"), std::string("a"));
    CHECK_EQ(*parse_flow_custom_btn_custom_id("flow-custom:b"), std::string("b"));
    CHECK_EQ(*parse_flow_modal_custom_id("flow-modal:c"), std::string("c"));
    CHECK(!parse_flow_modal_custom_id("flow-modalc"));
}

TEST_CASE("pure helpers match Node golden") {
    const Json f = w1d::load_fixture("discord_hook/pure.json");
    for (const auto& c : f["presets"]) {
        auto p = parse_settings_preset(c["input"].get<std::string>());
        Json out = Json::object();
        if (p.rounds) out["rounds"] = *p.rounds;
        if (p.turnsPerRound) out["turnsPerRound"] = *p.turnsPerRound;
        w1d::check_json_eq(out, c["output"], c["input"].get<std::string>().c_str());
    }
    for (const auto& c : f["mergeForumTags"]) {
        std::vector<ForumTag> existing;
        for (const auto& t : c["input"]) {
            ForumTag tag;
            tag.id = json_opt_string(t, "id");
            tag.name = t["name"].get<std::string>();
            tag.moderated = json_opt_bool(t, "moderated");
            if (t.contains("emoji")) {
                tag.emoji_id = json_opt_string(t["emoji"], "id");
                tag.emoji_name = json_opt_string(t["emoji"], "name");
            }
            existing.push_back(tag);
        }
        auto merged = merge_forum_tags(existing);
        CHECK_EQ(merged.added.size(), c["output"]["added"].size());
        for (std::size_t i = 0; i < merged.added.size(); ++i) CHECK_EQ(merged.added[i], c["output"]["added"][i].get<std::string>());
        CHECK_EQ(merged.tags.size(), c["output"]["tags"].size());
        for (std::size_t i = 0; i < merged.tags.size(); ++i) {
            const Json& e = c["output"]["tags"][i];
            CHECK_EQ(merged.tags[i].name, e["name"].get<std::string>());
            CHECK_EQ(merged.tags[i].id, json_opt_string(e, "id"));  // 稼働中スレッドのタグ ID を保つ
        }
    }
    for (const auto& c : f["humanize"]) CHECK_EQ(humanize_for_discord(c["input"].get<std::string>()), c["output"].get<std::string>());
    for (const auto& c : f["stripBotMention"])
        CHECK_EQ(strip_bot_mention(c["content"].get<std::string>(), json_opt_string(c, "botId")), c["output"].get<std::string>());
    for (const auto& c : f["normalizeDirective"]) {
        auto out = normalize_directive_text(c["input"].get<std::string>());
        if (c["output"].is_null()) CHECK(!out);
        else CHECK_EQ(*out, c["output"].get<std::string>());
    }
    for (const auto& c : f["directivesBlock"]) {
        std::vector<std::string> d;
        for (const auto& x : c["input"]) d.push_back(x.get<std::string>());
        CHECK_EQ(format_directives_block(d), c["output"].get<std::string>());
    }
    for (const auto& c : f["paperGapInstruction"]) {
        const Json& in = c["input"];
        PaperGapInstructionInput i{in["type"].get<std::string>(), json_opt_string(in, "target"), in["detail"].get<std::string>(), json_opt_string(in, "source")};
        CHECK_EQ(format_paper_gap_review_instruction(i), c["output"].get<std::string>());
    }
    for (const auto& c : f["normalize"]) {
        auto m = normalize_discord_inbound_message(c["input"]);
        if (c["output"].is_null()) CHECK(!m);
        else w1d::check_json_eq(to_json(*m), c["output"], "normalize");
    }
    // サロゲートペアの途中で切れる場合: JS は孤立サロゲートを残すが、C++ は正しい UTF-8 だけを返す
    {
        std::string emoji;
        for (int i = 0; i < 200; ++i) emoji += "😀";
        auto out = normalize_directive_text(emoji);
        CHECK(str::is_valid_utf8(*out));
        CHECK(str::js_length(*out) <= DIRECTIVE_TEXT_CAP);
        CHECK(str::ends_with(*out, "…"));
    }
    // 深い入れ子の event は打ち切る
    Json deep = Json::object();
    Json* cur = &deep;
    for (int i = 0; i < 40; ++i) cur = &((*cur)["event"] = Json::object());
    CHECK(!normalize_discord_inbound_message(deep));
}

TEST_CASE("paper gap types and modal ids") {
    CHECK(is_paper_gap_type("user_reaction"));
    CHECK(!is_paper_gap_type("unknown"));
    auto p = parse_paper_gap_modal_custom_id("paper-gap:user_reaction:ch123");
    CHECK(p && p->type == "user_reaction" && p->channelId == "ch123");
    CHECK(!parse_paper_gap_modal_custom_id("paper-gap:unknown:ch123"));
    CHECK(!parse_paper_gap_modal_custom_id("paper-gap:user_reaction:"));
    CHECK(!parse_paper_gap_modal_custom_id("flow-modal:ch123"));
    CHECK_EQ(paper_gap_type_choices().size(), std::size_t(6));
}

TEST_CASE("diagnostic reply matches Node, with byte limit boundary") {
    const Json f = w1d::load_fixture("discord_hook/diagnostic.json");
    auto small = diagnostic_reply(Json::parse(R"({"logic_differences":[],"design_gap":[]})"), "level-check.json");
    CHECK_EQ(small.content, f["small"]["content"].get<std::string>());
    CHECK(!small.file);
    CHECK_EQ(small.body()["allowed_mentions"].dump(), std::string(R"({"parse":[]})"));
    Json big = Json::parse(R"({"logic_differences":[{"kind":"revision"}],"design_gap":[]})");
    big["logic_differences"][0]["target"] = std::string(2000, 'x');
    auto r = diagnostic_reply(big, "mechanics-check.json");
    CHECK_EQ(r.content, f["big"]["content"].get<std::string>());
    CHECK_EQ(r.file->name, f["big"]["fileName"].get<std::string>());
    CHECK_EQ(r.file->data, f["big"]["fileText"].get<std::string>());
    for (const auto& c : f["failures"]) {
        design_diagnostic::DiagnosticError e(c["code"].get<std::string>(), "m");
        CHECK_EQ(diagnostic_failure(&e), c["output"].get<std::string>());
    }
    std::runtime_error other("x");
    CHECK_EQ(diagnostic_failure(&other), f["failureOther"].get<std::string>());
    // resultBytes ちょうどは通り、+1 は result_too_large
    const std::size_t limit = design_diagnostic::DIAGNOSTIC_LIMITS.resultBytes;
    auto sized = [](std::size_t total) {
        Json j = Json::parse(R"({"logic_differences":[],"design_gap":[],"pad":""})");
        const std::size_t base = json_stringify_pretty(j, 2).size();
        j["pad"] = std::string(total - base, 'a');
        return j;
    };
    CHECK(diagnostic_reply(sized(limit), "x.json").file.has_value());
    try {
        diagnostic_reply(sized(limit + 1), "x.json");
        CHECK(false);
    } catch (const design_diagnostic::DiagnosticError& e) {
        CHECK_EQ(e.code(), std::string("result_too_large"));
    }
}

TEST_CASE("paper review ack") {
    CHECK(std::string(PAPER_REVIEW_ACK_EMOJI) != "✅");
    std::vector<std::string> reacted;
    AckableMessage m{"ない", false, [&](const std::string& e) { reacted.push_back(e); }};
    CHECK(ack_paper_review_reply(m, true));
    CHECK_EQ(reacted, std::vector<std::string>{PAPER_REVIEW_ACK_EMOJI});
    reacted.clear();
    m.has_reference = true;
    CHECK(!ack_paper_review_reply(m, true));
    m.has_reference = false;
    CHECK(!ack_paper_review_reply(m, false));
    m.content = "   ";
    CHECK(!ack_paper_review_reply(m, true));
    CHECK(reacted.empty());
    std::vector<std::string> warns;
    AckableMessage failing{"開始", false, [](const std::string&) { throw std::runtime_error("Missing Permissions"); }};
    CHECK(ack_paper_review_reply(failing, true, [&](const std::string& w) { warns.push_back(w); }));
    CHECK(warns.size() == 1 && str::contains(warns[0], "Missing Permissions"));
}

TEST_CASE("extract game feedback") {
    Message msg;
    msg.content = "中毒性が高い";
    msg.timestamp = "2023-11-14T22:13:20.000Z";
    Channel ch;
    ch.id = "c";
    ch.type = channel_type::GuildText;
    ch.name = "vampire-survivors";
    ch.parent_id = "cat";
    Channel parent;
    parent.id = "cat";
    parent.type = channel_type::GuildCategory;
    parent.name = "ゲーム感想";
    auto ok = extract_game_feedback(msg, ch, parent, "ゲーム感想");
    CHECK(ok.has_value());
    CHECK_EQ(ok->gameTitle, std::string("vampire-survivors"));
    CHECK_EQ(ok->createdAt, 1'700'000'000'000.0);
    Channel other = parent;
    other.name = "雑談";
    CHECK(!extract_game_feedback(msg, ch, other, "ゲーム感想"));
    CHECK(!extract_game_feedback(msg, ch, std::nullopt, "ゲーム感想"));
    Channel thread = ch;
    thread.type = channel_type::PublicThread;
    CHECK(!extract_game_feedback(msg, thread, parent, "ゲーム感想"));
    msg.content = "   ";
    CHECK(!extract_game_feedback(msg, ch, parent, "ゲーム感想"));
}

DI_TEST_MAIN()
