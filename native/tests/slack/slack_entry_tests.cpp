// tests/slack-entry.test.ts の移植 + Block Kit の形を Node の出力 (fixtures/slack/blocks.json) と比べる。
#include "check.hpp"
#include "slack/slack_entry.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::slack;

TEST_CASE("parse_settings_preset") {
    auto d = parse_settings_preset("default");
    CHECK(!d.rounds && !d.turnsPerRound);
    CHECK(!parse_settings_preset("bogus").rounds);
    auto p = parse_settings_preset("2x4");
    CHECK_EQ(*p.rounds, 2.0);
    CHECK_EQ(*p.turnsPerRound, 4.0);
    CHECK_EQ(*parse_settings_preset("8x12").turnsPerRound, 12.0);
    CHECK(!parse_settings_preset("3x6x").rounds);
    CHECK(!parse_settings_preset("x").rounds);
}

TEST_CASE("is_thread_starter") {
    CHECK(is_thread_starter(Json::parse(R"({"type":"message","ts":"100.1","text":"テーマ"})")));
    CHECK(is_thread_starter(Json::parse(R"({"type":"message","ts":"100.1","thread_ts":"100.1"})")));
    CHECK(!is_thread_starter(Json::parse(R"({"type":"message","ts":"200.2","thread_ts":"100.1"})")));
    CHECK(!is_thread_starter(Json::parse(R"({"type":"message","ts":"100.1","subtype":"message_changed"})")));
    CHECK(!is_thread_starter(Json::parse(R"({"type":"message","ts":"100.1","bot_id":"B1"})")));
    CHECK(is_thread_starter(Json::parse(R"({"type":"message","ts":"100.1","bot_id":""})")));
    CHECK(!is_thread_starter(Json::parse(R"({"type":"reaction_added","ts":"100.1"})")));
    CHECK(!is_thread_starter(Json(nullptr)));
    CHECK(!is_thread_starter(Json::parse(R"({"type":"message"})")));
}

TEST_CASE("blocks match Node") {
    const Json f = w1d::load_fixture("slack/blocks.json");
    w1d::check_json_eq(Json(build_flow_pick_blocks()), f["pick"], "pick");
    w1d::check_json_eq(Json(build_flow_settings_blocks()), f["settings"], "settings");
    const auto pick = build_flow_pick_blocks();
    CHECK_EQ(pick[1]["elements"][0]["action_id"].get<std::string>(), std::string(SLACK_FLOW_PICK_ACTION));
    CHECK_EQ(build_flow_settings_blocks()[1]["elements"][0]["action_id"].get<std::string>(), std::string(SLACK_FLOW_SETTINGS_ACTION));
}

TEST_CASE("payload extraction") {
    const Json payload = Json::parse(R"({"type":"block_actions","actions":[{"action_id":"slack-flow-pick","selected_option":{"value":"改善"}}]})");
    CHECK_EQ(*first_action_id(payload), std::string(SLACK_FLOW_PICK_ACTION));
    CHECK_EQ(*first_selected_value(payload), std::string("改善"));
    CHECK(!first_action_id(Json::parse(R"({"actions":[]})")));
    CHECK(!first_selected_value(Json::parse(R"({"actions":[]})")));
    CHECK(!first_selected_value(Json::parse(R"({"actions":[{"selected_option":{"value":3}}]})")));
    CHECK(!first_action_id(Json::parse(R"({"actions":"x"})")));
}

DI_TEST_MAIN()
