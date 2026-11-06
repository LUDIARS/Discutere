// w3m_discord のテスト (3/4)。準備は w3m_discord_support.hpp。
#include "w3m_discord_support.hpp"

using namespace w3m_discord_support;

TEST_CASE("RestInteractionResponder: 偽 Discord に deferred(ephemeral) と @original 編集 (添付は multipart) を送る") {
    FakeDiscord d;
    DiscordApi api(d.options());
    const auto lreq = fixture_case("w2j/level_requests.json", "base")["input"];
    const auto lres = fixture_case("w2j/level_responses.json", "valid")["text"].get<std::string>();
    const Json raw{{"id", "i1"},
                   {"token", "fake-interaction-token"},
                   {"application_id", "app"},
                   {"type", 2},
                   {"guild_id", "g"},
                   {"channel_id", "c"},
                   {"member", {{"user", {{"id", "u1"}}}}},
                   {"data",
                    {{"name", "level-check"},
                     {"options", Json::array({Json{{"name", "level"}, {"type", 3}, {"value", lreq["levelText"]}},
                                              Json{{"name", "baseline"}, {"type", 3}, {"value", lreq["baselineText"]}},
                                              Json{{"name", "input"}, {"type", 3},
                                                   {"value", json_stringify(Json{{"references", lreq["references"]}, {"playModel", lreq["playModel"]}})}}})}}}};
    const auto inter = slash_interaction_from_json(raw);
    CHECK_EQ(inter.userId.value_or(""), std::string("u1"));
    CHECK_EQ(inter.get_string("level").value_or(""), lreq["levelText"].get<std::string>());
    CHECK(!inter.get_string("missing").has_value());
    CHECK(!inter.get_boolean("level").has_value());
    RestInteractionResponder responder(api, inter);
    design_diagnostic::DiagnosticDependencies deps{llm_text(lres), std::nullopt};
    CHECK(handle_level_check_command(inter, responder, &deps));
    CHECK_EQ(d.callbacks.size(), std::size_t(1));
    CHECK_EQ(d.callbacks[0], std::string(R"({"type":5,"data":{"flags":64}})"));
    CHECK_EQ(d.edits.size(), std::size_t(1));
    CHECK(str::starts_with(d.edits[0].first, "multipart/form-data"));
    CHECK(str::contains(d.edits[0].second, "level-check.json"));
    CHECK(str::contains(d.edits[0].second, R"("allowed_mentions":{"parse":[]})"));
    CHECK_THROWS(slash_interaction_from_json(Json{{"id", "x"}}));
    CHECK_THROWS(slash_interaction_from_json(Json::array()));
}

// ─── discussion-bridge ───────────────────────────────────────────

TEST_CASE("discussion-bridge: headless は skipped・scene 不正・token 無し・webhook 投稿・thread・bot への fallback") {
    FakeDiscord d;
    {
        Env env;  // token 無し
        auto c = env.core();
        const auto headless = make_session(*c, "gap:g1");
        auto r = post_discussion_to_discord({c.get(), "knowledge", headless, "utterance", "司会 結", "本文", true});
        CHECK(!r.ok);
        CHECK(r.skipped);
        CHECK_EQ(json_stringify(to_json(r)), std::string(R"J({"ok":false,"skipped":true,"reason":"session.scene is not discord-bound (headless)"})J"));
        CHECK(post_discussion_to_discord({c.get(), "knowledge", "missing", "utterance", "x", "y", true}).skipped);
        const auto bad = make_session(*c, "discord:1");
        CHECK_EQ(post_discussion_to_discord({c.get(), "knowledge", bad, "utterance", "x", "y", true}).reason.value_or(""),
                 std::string("session.scene channel parse failed"));
        const auto ok_scene = make_session(*c, "discord:1/2");
        r = post_discussion_to_discord({c.get(), "knowledge", ok_scene, "utterance", "x", "y", true});
        CHECK(!r.ok);
        CHECK(!r.skipped);
        CHECK_EQ(r.reason.value_or(""), std::string("no discord bot token (channel_monitors / config どちらも未設定)"));
    }
    {
        Env env(std::string("config-token-not-real"));
        std::vector<std::string> tokens;
        set_discussion_bridge_api_factory([&](const std::string& token) {
            tokens.push_back(token);
            auto o = d.options();
            o.token = token;
            return std::make_shared<DiscordApi>(o);
        });
        // channel_monitors の discord 登録 (guild 一致) を優先する
        db::MonitorRepo().create(Json{{"id", "m1"},
                                      {"workspaceId", "knowledge"},
                                      {"platform", "discord"},
                                      {"channelId", "c"},
                                      {"channelName", "n"},
                                      {"botToken", "monitor-token-not-real"},
                                      {"botWorkspaceId", "1"},
                                      {"createdBy", "t"}});
        auto c = env.core();
        const auto s = make_session(*c, "discord:1/thread");
        auto r = post_discussion_to_discord({c.get(), "knowledge", s, "utterance", "費田 渋", "それは違う。次へ!", true});
        CHECK(r.ok);
        CHECK_EQ(r.channelId.value_or(""), std::string("thread"));
        CHECK_EQ(r.messageId.value_or(""), std::string("wm1"));
        CHECK_EQ(tokens.back(), std::string("monitor-token-not-real"));
        CHECK_EQ(d.webhook_posts.back()["username"], Json("費田 渋"));
        CHECK_EQ(d.webhook_posts.back()["content"], Json(humanize_for_discord("それは違う。次へ!")));
        CHECK(str::contains(d.webhook_posts.back()["_query"].get<std::string>(), "thread_id=thread"));
        // 別 guild は monitor を使わず config の token
        const auto s2 = make_session(*c, "discord:7/chan");
        clear_webhook_cache();
        d.webhook_forbidden = true;  // webhook 権限なし → bot 直接投稿 (名前を本文先頭に)
        r = post_discussion_to_discord({c.get(), "knowledge", s2, "utterance", "論者", "意見", true});
        CHECK(r.ok);
        CHECK_EQ(tokens.back(), std::string("config-token-not-real"));
        CHECK_EQ(d.created_messages.back()["content"], Json("**論者**\n" + humanize_for_discord("意見")));
        // viaWebhook=false は bot 名義でそのまま
        r = post_discussion_to_discord({c.get(), "knowledge", s2, "hypothesis", "論者", "仮説", false});
        CHECK(r.ok);
        CHECK_EQ(d.created_messages.back()["content"], Json(humanize_for_discord("仮説")));
        // token は結果に出ない
        CHECK(!str::contains(json_stringify(to_json(r)), "not-real"));
        d.webhook_forbidden = false;
    }
}

// ─── directive-handler ───────────────────────────────────────────

TEST_CASE("directive-handler: 空文は使い方・議論なしは案内・進行中の gap に保存して了解を返す") {
    Env env;
    auto r = handle_directive_message({"knowledge", "1", "2", "   ", std::nullopt});
    CHECK(!r.stored);
    CHECK(str::contains(r.reply, "進行の調整指示を受け付けます"));
    r = handle_directive_message({"knowledge", "1", "2", "もっと簡単に", std::string("u1")});
    CHECK(!r.stored);
    CHECK(str::contains(r.reply, "進行中の議論が見つかりませんでした"));
    auto c = env.core();
    core::repositories::DesignGapInput g;
    g.workspaceId = "knowledge";
    g.title = "議題";
    g.status = "open";
    const auto gap = c->repos.designGap.create(g);
    make_session(*c, "discord:1/2", "discussion-of-gap:" + gap);
    c.reset();
    r = handle_directive_message({"knowledge", "1", "2", "  もっと簡単に  ", std::string("u1")});
    CHECK(r.stored);
    CHECK_EQ(r.gapId.value_or(""), gap);
    CHECK_EQ(r.reply, std::string("🛠️ 了解です。「もっと簡単に」 を進行に反映します。 これからの発言とまとめに効いてきます。"));
    c = env.core();
    const auto list = list_facilitator_directives(c->raw(), "knowledge", gap);
    CHECK_EQ(list.size(), std::size_t(1));
    CHECK_EQ(list[0], std::string("もっと簡単に"));
    // 巨大な指示は上限 (300 UTF-16) で切って保存
    r = handle_directive_message(c->raw(), {"knowledge", "1", "2", std::string(100'000, 'x'), std::nullopt});
    CHECK(r.stored);
    CHECK_EQ(str::js_length(list_facilitator_directives(c->raw(), "knowledge", gap).back()), DIRECTIVE_TEXT_CAP);
}

DI_TEST_MAIN()
