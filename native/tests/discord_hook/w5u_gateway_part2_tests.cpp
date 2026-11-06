// w5u_gateway のテスト (2/4)。準備は w5u_gateway_support.hpp。
#include "w5u_gateway_support.hpp"

using namespace w5u_gateway_support;

TEST_CASE("THREAD_CREATE: 管理フォーラムの starter でフロー起動 → webhook → 結論 → まとめ転記 + lock/archive") {
    GwHarness h;
    auto gw = h.start(h.deps());
    h.discord.add_thread("T1", "F1", "周回の議論 ラウンド:2");
    h.discord.add_starter("T1", "周回の本文");
    gw->handle_dispatch("THREAD_CREATE", thread_create("T1", "F1", "周回の議論 ラウンド:2", {h.tag_id("議論"), h.tag_id("機密")}));
    h.settle(*gw);
    CHECK_EQ(h.dispatch_count(), std::size_t{1});
    CHECK_EQ(h.dispatched[0].theme, std::string("周回の議論 ラウンド:2"));
    CHECK_EQ(h.dispatched[0].rounds.value_or(0), 2.0);
    CHECK(h.dispatched[0].tags == std::vector<std::string>{"機密"});
    CHECK_EQ(h.dispatched[0].scene.value_or(""), std::string("discord:G1/T1"));
    const auto posts = h.discord.post_contents("T1");
    CHECK(!posts.empty() && str::starts_with(posts[0], "🗣️ **企画/議論**"));
    CHECK_EQ(posts.back(), std::string("✅ **結論**\n短くする"));
    CHECK_EQ(h.discord.snapshot(h.discord.webhooks).size(), std::size_t{1});
    // まとめ投稿へ転記し、スレッドを lock + archive。
    CHECK(!h.discord.post_contents("sum").empty());
    bool locked = false, archived = false;
    for (const auto& p : h.discord.snapshot(h.discord.patches)) {
        if (p["channel"] != "T1") continue;
        locked = locked || p["body"].value("locked", false);
        archived = archived || p["body"].value("archived", false);
    }
    CHECK(locked);
    CHECK(archived);
}

TEST_CASE("THREAD_CREATE: 管理外フォーラム・newly_created でない・bot の starter・親が forum でないものは無視") {
    GwHarness h;
    auto gw = h.start(h.deps());
    h.discord.add_thread("TX", "F9");
    h.discord.add_starter("TX", "本文");
    gw->handle_dispatch("THREAD_CREATE", thread_create("TX", "F9", "管理外 ラウンド:2", {h.tag_id("議論")}));
    h.discord.add_thread("TN");
    h.discord.add_starter("TN", "本文");
    gw->handle_dispatch("THREAD_CREATE", thread_create("TN", "F1", "既存 ラウンド:2", {h.tag_id("議論")}, false));
    h.discord.add_thread("TB");
    h.discord.add_starter("TB", "bot の投稿", true);
    gw->handle_dispatch("THREAD_CREATE", thread_create("TB", "F1", "bot ラウンド:2", {h.tag_id("議論")}));
    // 偽の guild_id を名乗っても REST の同一性で落ちる (F1 は G1 のフォーラム)
    Json spoof = thread_create("TS", "F1", "なりすまし ラウンド:2", {h.tag_id("議論")});
    spoof["guild_id"] = "G2";
    gw->handle_dispatch("THREAD_CREATE", spoof);
    gw->handle_dispatch("THREAD_CREATE", Json::array());  // 形の壊れた d
    h.settle(*gw);
    CHECK_EQ(h.dispatch_count(), std::size_t{0});
    CHECK(h.discord.post_contents().empty());
}

TEST_CASE("議論タイプ未指定 → flow-pick メニュー → 選択 → 進行量メニュー → プリセットで起動・期限切れ・別スレッドの component は無視") {
    GwHarness h;
    auto gw = h.start(h.deps());
    h.discord.add_thread("T3", "F1", "タグなし");
    h.discord.add_starter("T3", "本文");
    gw->handle_dispatch("THREAD_CREATE", thread_create("T3", "F1", "タグなし", {}));
    h.settle(*gw);
    auto posts = h.discord.snapshot(h.discord.posts);
    CHECK_EQ(posts.size(), std::size_t{1});
    CHECK(str::starts_with(posts[0]["content"].get<std::string>(), "🏷️ 議論タイプのタグが付いていません"));
    CHECK_EQ(posts[0]["components"][0]["components"][0]["custom_id"].get<std::string>(), std::string("flow-pick:T3"));

    // 別スレッド (T4) から T3 の component を操作しても門で落ちる。
    h.discord.add_thread("T4");
    gw->handle_dispatch("INTERACTION_CREATE", component("i0", "T4", 3, "flow-pick:T3", {"議論"}));
    h.settle(*gw);
    CHECK(callback_for(h.discord, "i0").is_null());

    gw->handle_dispatch("INTERACTION_CREATE", component("i1", "T3", 3, "flow-pick:T3", {"議論"}));
    h.settle(*gw);
    const auto cb = callback_for(h.discord, "i1");
    CHECK_EQ(cb["type"].get<int>(), 7);
    CHECK_EQ(cb["data"]["content"].get<std::string>(), std::string("✅ 「議論」を選択"));
    posts = h.discord.snapshot(h.discord.posts);
    CHECK(str::starts_with(posts.back()["content"].get<std::string>(), "⚙️ 進行量を選んで"));
    CHECK_EQ(h.dispatch_count(), std::size_t{0});

    // 同じ pick をもう一度 → 期限切れ (ephemeral)
    gw->handle_dispatch("INTERACTION_CREATE", component("i2", "T3", 3, "flow-pick:T3", {"議論"}));
    h.settle(*gw);
    const auto expired = callback_for(h.discord, "i2");
    CHECK_EQ(expired["data"]["content"].get<std::string>(), std::string("この選択は期限切れです。スレッドを作り直してください。"));
    CHECK_EQ(expired["data"]["flags"].get<int>(), 64);

    // 「数値を指定」ボタン → モーダル
    gw->handle_dispatch("INTERACTION_CREATE", component("i3", "T3", 2, "flow-custom:T3"));
    h.settle(*gw);
    CHECK_EQ(callback_for(h.discord, "i3")["type"].get<int>(), 9);

    gw->handle_dispatch("INTERACTION_CREATE", component("i4", "T3", 3, "flow-settings:T3", {"3x6"}));
    h.settle(*gw);
    CHECK_EQ(callback_for(h.discord, "i4")["data"]["content"].get<std::string>(), std::string("✅ 進行量「3x6」で開始します"));
    CHECK_EQ(h.dispatch_count(), std::size_t{1});
    CHECK_EQ(h.dispatched[0].rounds.value_or(0), 3.0);
    CHECK_EQ(h.dispatched[0].turnsPerRound.value_or(0), 6.0);

    // 使い終わった設定のボタン → 期限切れ
    gw->handle_dispatch("INTERACTION_CREATE", component("i5", "T3", 2, "flow-custom:T3"));
    h.settle(*gw);
    CHECK_EQ(callback_for(h.discord, "i5")["data"]["content"].get<std::string>(), std::string("この設定は期限切れです。スレッドを作り直してください。"));
}

TEST_CASE("進行量モーダル: 数値で起動 (不正な値は既定)") {
    GwHarness h;
    auto gw = h.start(h.deps());
    h.discord.add_thread("T5", "F1", "モーダル");
    h.discord.add_starter("T5", "本文");
    gw->handle_dispatch("THREAD_CREATE", thread_create("T5", "F1", "モーダル", {h.tag_id("改善")}));
    h.settle(*gw);
    Json modal{{"id", "im"}, {"token", "itok"}, {"application_id", "app"}, {"type", 5}, {"guild_id", "G1"}, {"channel_id", "T5"},
               {"data", Json{{"custom_id", "flow-modal:T5"},
                             {"components", Json::array({Json{{"components", Json::array({Json{{"custom_id", "rounds"}, {"value", "4"}}})}},
                                                         Json{{"components", Json::array({Json{{"custom_id", "turns"}, {"value", "-2"}}})}}})}}}};
    gw->handle_dispatch("INTERACTION_CREATE", modal);
    h.settle(*gw);
    const auto cb = callback_for(h.discord, "im");
    CHECK_EQ(cb["data"]["content"].get<std::string>(), std::string("✅ 進行量 (ラウンド 4 × ターン 既定) で開始します"));
    CHECK(!cb["data"].contains("flags"));
    CHECK_EQ(h.dispatch_count(), std::size_t{1});
    CHECK_EQ(h.dispatched[0].flow, std::string("improvement"));
    CHECK(!h.dispatched[0].turnsPerRound);
}

DI_TEST_MAIN()
