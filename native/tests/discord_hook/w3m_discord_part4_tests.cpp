// w3m_discord のテスト (4/4)。準備は w3m_discord_support.hpp。
#include "w3m_discord_support.hpp"

using namespace w3m_discord_support;

// ─── monitor-card ────────────────────────────────────────────────

TEST_CASE("monitor-card: embed が Node と同じ (golden)") {
    for (const auto& c : test::load_fixture("w3m/monitor_embeds.json")) {
        const Json body{{"embeds", Json::array({build_monitor_embed(c["active"].get<std::int64_t>(), c["pending"].get<std::int64_t>(),
                                                                    c["at"].get<std::int64_t>())})}};
        CHECK_EQ(json_stringify(body), c["json"].get<std::string>());
    }
}

TEST_CASE("monitor-card: カテゴリとチャンネルを作り、カードを送り、以後は編集・重複カードは削除・guild 不在は何もしない") {
    FakeDiscord d;
    Env env;
    {
        auto c = env.core();
        make_session(*c, "discord:1/2");
    }
    auto api = std::make_shared<DiscordApi>(d.options());
    MonitorCard card({api, "knowledge", "g"});
    card.tick();
    CHECK_EQ(d.channels.size(), std::size_t(2));
    CHECK_EQ(d.channels[0]["name"], Json("状態"));
    CHECK_EQ(d.channels[0]["type"], Json(channel_type::GuildCategory));
    CHECK_EQ(d.channels[1]["name"], Json("discutere-monitor"));
    CHECK_EQ(d.channels[1]["parent_id"], d.channels[0]["id"]);
    CHECK_EQ(d.created_messages.size(), std::size_t(1));
    const auto& embed = d.created_messages[0]["_body"]["embeds"][0];
    CHECK_EQ(embed["fields"][1]["value"], Json("1"));
    CHECK_EQ(embed["fields"][0]["value"], Json("アクティブ"));
    // 2 回目: 同じカードを編集 (チャンネルは作り直さない)
    card.tick();
    CHECK_EQ(d.channels.size(), std::size_t(2));
    CHECK_EQ(d.created_messages.size(), std::size_t(1));
    CHECK_EQ(d.edited_messages.size(), std::size_t(1));

    // 再起動相当: 既存のカードが 2 枚 (新しい方を残し古い方を消す)、他人のメッセージは無視
    d.channel_messages = Json::array({
        Json{{"id", "1000"}, {"channel_id", "c"}, {"author", {{"username", "u"}, {"id", "bot"}}}, {"embeds", Json::array({Json{{"title", "🟢 Discutere Monitor"}}})}},
        Json{{"id", "3000"}, {"channel_id", "c"}, {"author", {{"username", "u"}, {"id", "bot"}}}, {"embeds", Json::array({Json{{"title", "🟢 Discutere Monitor"}}})}},
        Json{{"id", "9000"}, {"channel_id", "c"}, {"author", {{"username", "u"}, {"id", "human"}}}, {"embeds", Json::array({Json{{"title", "Discutere Monitor"}}})}},
        Json{{"id", "8000"}, {"channel_id", "c"}, {"author", {{"username", "u"}, {"id", "bot"}}}, {"embeds", Json::array()}},
    });
    MonitorCard card2({api, "knowledge", "g"});
    card2.tick();
    CHECK_EQ(d.deleted.size(), std::size_t(1));
    CHECK_EQ(d.deleted[0], std::string("1000"));
    CHECK_EQ(d.edited_messages.size(), std::size_t(2));
    CHECK_EQ(d.created_messages.size(), std::size_t(1));

    d.guild_missing = true;
    MonitorCard card3({api, "knowledge", "g"});
    card3.tick();
    CHECK_EQ(d.edited_messages.size(), std::size_t(2));
    CHECK_EQ(d.created_messages.size(), std::size_t(1));
    d.guild_missing = false;

    // start: 即時 1 回 + 周期、stop 後は止まる
    d.channel_messages = Json::array();
    MonitorCard card4({api, "knowledge", "g"});
    auto sent = [&] {
        std::lock_guard l(d.m);
        return d.created_messages.size() + d.edited_messages.size();
    };
    const auto before = sent();
    card4.start(20);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (sent() < before + 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    card4.stop();
    CHECK(sent() >= before + 2);
}

DI_TEST_MAIN()
