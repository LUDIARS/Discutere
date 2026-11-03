// w5-U: discord-hook/gateway.ts の移植の検証。偽の Discord REST (loopback) と偽の Gateway (WebSocket) で:
//   - READY → slash 登録・運用チャンネル・議論フォーラムの ensure → 管理フォーラム ID の解決
//   - THREAD_CREATE → フロー起動 → webhook 投稿 → 結論 → まとめ転記 + lock/archive (偽 Gateway 経由の E2E)
//   - 管理外フォーラム・newly_created でないスレッド・別スレッドを指す component は無視
//   - 議論タイプ選択メニュー (flow-pick) / 進行量 (flow-settings select・flow-custom ボタン・flow-modal)
//   - admin コマンドの拒否 (allowlist 未設定 / 不一致)・管理外チャンネルの slash は応答しない
//   - ✅ 承認 (bot 自身・他 bot の ✅ は無視)・壁打ちの継続・reply 投稿は無視・bot メンションの調整指示
//   - 起動系キューと通常キューの分離 (通常が満杯でも起動系は落ちない)・token をログに出さない
#include "check.hpp"
#include "w5u_fake_gateway.hpp"
#include "w5u_support.hpp"

#include "base/log.hpp"
#include "base/strings.hpp"
#include "discord_hook/gateway.hpp"
#include "flow/discord_live.hpp"
#include "flow/discussion_paper.hpp"

using namespace di;
using namespace di::discord_hook;
using w5u::Json;

namespace {

constexpr const char* kToken = "fake-bot-token-not-real";

struct LogCapture {
    std::mutex m;
    std::string all;
    int id;
    LogCapture() {
        id = log::add_sink([this](const log::Record& r) {
            std::lock_guard lock(m);
            all.append(r.message);
            all.push_back('\n');
        });
    }
    ~LogCapture() { log::remove_sink(id); }
    bool contains(const std::string& s) {
        std::lock_guard lock(m);
        return all.find(s) != std::string::npos;
    }
};

struct GwHarness {
    w5u::TempEnv env;
    w5u::FakeDiscord discord;
    Executor bg{"w5u-gw-bg", 2, 64};
    std::shared_ptr<w5u::FakeLLM> llm = std::make_shared<w5u::FakeLLM>();
    std::mutex m;
    std::vector<flow::DispatchInput> dispatched;
    std::vector<ReactionInfo> reactions_seen;
    bool review = false;

    explicit GwHarness(std::map<std::string, std::string> e = {}) : env(e) {
        flow::reset_flow_live_for_test();
        review = e.count("DISCUTERE_FLOW_PAPER_REVIEW_ENABLED") > 0;
        // 壁打ちタグ (廃止タグだが入口の解析は注入するので試せる)
        std::lock_guard lock(discord.m);
        discord.channels["F1"]["available_tags"].push_back(Json{{"id", "tag-spar"}, {"name", "壁打ち"}});
    }

    std::string tag_id(const std::string& name) {
        std::lock_guard lock(discord.m);
        for (const auto& t : discord.channels["F1"]["available_tags"])
            if (t["name"] == name) return t["id"];
        return "";
    }

    DiscordGatewayDeps deps() {
        DiscordGatewayDeps d;
        d.router.workspaceId = "ws";
        d.botToken = kToken;
        d.applicationId = "app";
        d.guildIds = {"G1", "dm", ""};
        ForumConfig f;
        f.enabled = true;
        f.summaryChannelName = "まとめ投稿";
        f.dataLearningChannelName = "データ学習依頼";
        f.managedCategoryName = "システム";
        f.discussionForumName = "議論";
        d.forum = f;
        flow::FlowDiscordDeps fl;
        fl.llm = llm;
        fl.gamesDir = env.games();
        fl.voiceRequest = user_voices_fake();
        fl.submitBackground = [this](std::function<void()> t) { return bg.submit(std::move(t)); };
        fl.prepareUserVoices = [](const flow::user_voices::PrepareUserVoicesArgs&) { return flow::user_voices::PreparedUserVoices{}; };
        fl.expandNotionLinks = [](const std::string&) { return std::string(); };
        fl.listExternalVoices = [](const std::vector<std::string>&, int) { return std::vector<flow::ContextVoice>{flow::ContextVoice{"声", "steam", {}}}; };
        fl.dispatch = [this](const flow::DispatchInput& in, const flow::DispatchDeps& dd) {
            {
                std::lock_guard lock(m);
                dispatched.push_back(in);
            }
            if (in.flow == "sparring") {
                // TS の parseFlowKind は壁打ちのラベルを持たないので、壁打ち経路は session を直接作って確かめる。
                flow::SparringDeps sd;
                sd.llm = dd.llm;
                sd.onUtterance = dd.onUtterance;
                sd.gamesDir = dd.gamesDir;
                auto session = std::make_shared<flow::SparringSession>(in.theme, in.tags, sd);
                session->start();
                flow::DispatchResult r;
                r.kind = "sparring";
                r.flow = "sparring";
                r.session = session;
                return r;
            }
            if (dd.sessionId) {
                flow::NewPaper p;
                p.sessionId = *dd.sessionId;
                p.theme = in.theme;
                flow::persist_paper(p, in.flow);
            }
            flow::FlowUtteranceRecord u;
            u.id = "u1";
            u.personaName = "相馬";
            u.role = "debater";
            u.stance = "pro";
            u.text = "周回は短いほうがいい。";
            if (dd.onUtterance) dd.onUtterance(u);
            flow::DispatchResult r;
            r.kind = in.flow;
            r.flow = in.flow;
            flow::FlowDirectorResult res;
            res.concluded = true;
            res.conclusion = "短くする";
            r.result = res;
            return r;
        };
        d.flowLive = fl;
        d.entryParsers.parseForumEntry = [](const std::vector<std::string>& names) {
            ParsedForumEntry e;
            for (const auto& n : names) {
                if (n == "議論") e.flow = "discussion";
                else if (n == "改善") e.flow = "improvement";
                else if (n == "壁打ち") e.flow = "sparring";
                else if (n == "機密" || n == "内部") e.tags.push_back(n);
            }
            return e;
        };
        d.entryParsers.parseRoundsTurns = [](const std::string& text) {
            ParsedRoundsTurns rt;
            if (text.find("ラウンド:2") != std::string::npos) rt.rounds = 2;
            return rt;
        };
        d.onReaction = [this](const ReactionInfo& r) {
            std::lock_guard lock(m);
            reactions_seen.push_back(r);
        };
        d.rest = discord.options();
        d.connect = false;
        d.monitorCards = false;
        d.sleep = [](std::chrono::milliseconds) {};
        return d;
    }

    static flow::user_voices::VoiceRequestPort user_voices_fake() {
        flow::user_voices::VoiceRequestPort p;
        p.submit = [](const std::string& id, const std::string&) { return flow::user_voices::VoiceRequestReceipt{true, id}; };
        p.status = [](const std::string& id) { return flow::user_voices::VoiceRequestReceipt{true, id}; };
        return p;
    }

    std::unique_ptr<DiscordGateway> start(DiscordGatewayDeps d) {
        auto gw = start_discord_gateway(std::move(d));
        CHECK(gw != nullptr);
        gw->handle_dispatch("READY", Json{{"user", Json{{"id", "bot"}, {"username", "di-bot"}}}, {"application", Json{{"id", "app"}}},
                                          {"session_id", "s"}});
        CHECK(w5u::wait_until([&] { return gw->ready_done(); }));
        return gw;
    }

    void settle(DiscordGateway& gw) {
        for (int i = 0; i < 3; ++i) {
            CHECK(gw.wait_idle(std::chrono::milliseconds(10'000)));
            CHECK(w5u::drain(bg));
        }
    }

    std::size_t dispatch_count() {
        std::lock_guard lock(m);
        return dispatched.size();
    }
};

Json thread_create(const std::string& id, const std::string& parent, const std::string& name, const std::vector<std::string>& tags,
                   bool newly = true) {
    Json d = w5u::channel_json(id, 11, "G1", parent, name);
    d["applied_tags"] = tags;
    d["newly_created"] = newly;
    return d;
}

Json message(const std::string& id, const std::string& channel, const std::string& content, const std::string& author = "u1",
             bool bot = false) {
    return Json{{"id", id}, {"channel_id", channel}, {"guild_id", "G1"}, {"content", content}, {"timestamp", "2026-01-01T00:00:00.000Z"},
                {"author", Json{{"id", author}, {"username", "alice"}, {"bot", bot}}}, {"mentions", Json::array()}};
}

Json component(const std::string& id, const std::string& channel, int component_type, const std::string& custom_id,
               const std::vector<std::string>& values = {}) {
    Json data{{"custom_id", custom_id}, {"component_type", component_type}};
    if (!values.empty()) data["values"] = values;
    return Json{{"id", id}, {"token", "itok-" + id}, {"application_id", "app"}, {"type", 3}, {"guild_id", "G1"}, {"channel_id", channel},
                {"member", Json{{"user", Json{{"id", "u1"}}}}}, {"data", data}};
}

Json slash(const std::string& id, const std::string& channel, const std::string& name, const std::string& user, Json options = Json::array()) {
    return Json{{"id", id}, {"token", "itok-" + id}, {"application_id", "app"}, {"type", 2}, {"guild_id", "G1"}, {"channel_id", channel},
                {"member", Json{{"user", Json{{"id", user}}}}}, {"data", Json{{"name", name}, {"options", options}}}};
}

Json callback_for(w5u::FakeDiscord& d, const std::string& id) {
    for (const auto& c : d.snapshot(d.callbacks))
        if (c["id"] == id) return c["body"];
    return nullptr;
}

} // namespace

TEST_CASE("純関数: テキスト系添付・モーダルの値・進行量の数値") {
    CHECK(is_text_like_attachment("spec.MD", std::nullopt));
    CHECK(is_text_like_attachment("a.yml", std::nullopt));
    CHECK(is_text_like_attachment("x.bin", std::string("text/plain")));
    CHECK(is_text_like_attachment("x.bin", std::string("application/json; charset=utf-8")));
    CHECK(!is_text_like_attachment("x.png", std::string("image/png")));
    CHECK(!is_text_like_attachment("x.mdx", std::nullopt));
    CHECK(!is_text_like_attachment("x.bin", std::string("Text/plain")));  // startsWith は大文字小文字を区別する
    const Json data{{"custom_id", "flow-modal:T1"},
                    {"components", Json::array({Json{{"type", 1}, {"components", Json::array({Json{{"type", 4}, {"custom_id", "rounds"}, {"value", " 3 "}}})}},
                                                Json{{"type", 18}, {"component", Json{{"type", 4}, {"custom_id", "turns"}, {"value", "6"}}}}})}};
    const auto f = modal_field_values(data);
    CHECK_EQ(f.at("rounds"), std::string(" 3 "));
    CHECK_EQ(f.at("turns"), std::string("6"));
    CHECK(modal_field_values(Json::array()).empty());
    CHECK_EQ(positive_number_field(" 3 ").value_or(0), 3.0);
    CHECK_EQ(positive_number_field("2.5").value_or(0), 2.5);
    CHECK(!positive_number_field(""));
    CHECK(!positive_number_field("0"));
    CHECK(!positive_number_field("-1"));
    CHECK(!positive_number_field("abc"));
    CHECK(!positive_number_field("Infinity"));
    // 深い入れ子 (上限 8 より深い) は辿らない
    Json deep{{"custom_id", "x"}, {"value", "v"}};
    for (int i = 0; i < 20; ++i) deep = Json{{"components", Json::array({deep})}};
    CHECK(modal_field_values(deep).empty());
}

TEST_CASE("bot token が空なら起動しない") {
    DiscordGatewayDeps d;
    CHECK(start_discord_gateway(std::move(d)) == nullptr);
}

TEST_CASE("READY: slash 登録・運用チャンネル・議論フォーラムの解決 (READY は 1 度だけ)") {
    GwHarness h;
    auto gw = h.start(h.deps());
    const auto ids = gw->managed_forum_ids();
    CHECK_EQ(ids.size(), std::size_t{1});
    CHECK_EQ(ids.at("G1"), std::string("F1"));
    CHECK_EQ(h.discord.commands_registered, 1);
    const auto crawl = gw->crawl_channel_ids();
    CHECK(std::find(crawl.begin(), crawl.end(), "dl") != crawl.end());
    // 再 identify の READY では繰り返さない (discord.js の client.once)。
    gw->handle_dispatch("READY", Json{{"user", Json{{"id", "bot"}}}});
    h.settle(*gw);
    CHECK_EQ(h.discord.commands_registered, 1);
}

TEST_CASE("READY: 議論フォーラム名が空なら guild を拒否し、全イベントが門で落ちる (fail closed)") {
    GwHarness h;
    auto d = h.deps();
    d.forum->discussionForumName = "  ";
    auto gw = h.start(std::move(d));
    CHECK(gw->managed_forum_ids().empty());
    h.discord.add_thread("T1");
    h.discord.add_starter("T1", "本文");
    gw->handle_dispatch("THREAD_CREATE", thread_create("T1", "F1", "周回 ラウンド:2", {h.tag_id("議論")}));
    gw->handle_dispatch("INTERACTION_CREATE", slash("i1", "T1", "discutere-status", "u1"));
    h.settle(*gw);
    CHECK_EQ(h.dispatch_count(), std::size_t{0});
    CHECK(h.discord.snapshot(h.discord.callbacks).empty());
    CHECK(gw->event_stats().gateRejected >= 2);
}

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

TEST_CASE("admin コマンド: allowlist 未設定は全拒否・不一致は admin only・管理外チャンネルの slash は応答しない") {
    GwHarness h;
    auto gw = h.start(h.deps());
    h.discord.add_thread("T1");
    gw->handle_dispatch("INTERACTION_CREATE", slash("a1", "T1", "discutere-kill", "u1"));
    h.settle(*gw);
    auto cb = callback_for(h.discord, "a1");
    CHECK_EQ(cb["data"]["content"].get<std::string>(), std::string("⚠️ discord.adminIds not set — admin commands are disabled"));
    CHECK_EQ(cb["data"]["flags"].get<int>(), 64);
    gw->stop();

    GwHarness h2;
    auto d = h2.deps();
    d.router.adminIds = {"admin1"};
    auto gw2 = h2.start(std::move(d));
    h2.discord.add_thread("T1");
    h2.discord.add_thread("TX", "F9");
    gw2->handle_dispatch("INTERACTION_CREATE", slash("a2", "T1", "discutere-kill", "u1"));
    gw2->handle_dispatch("INTERACTION_CREATE", slash("a3", "TX", "discutere-kill", "admin1"));
    h2.settle(*gw2);
    CHECK_EQ(callback_for(h2.discord, "a2")["data"]["content"].get<std::string>(), std::string("⚠️ admin only"));
    CHECK(callback_for(h2.discord, "a3").is_null());  // 管理外フォーラムのスレッド
}

TEST_CASE("ペーパーレビュー: 返信に ☑️ → 調整、✅ 承認 (bot 自身・他 bot の ✅ は無視)、reply 投稿は無視") {
    GwHarness h({{"DISCUTERE_FLOW_PAPER_REVIEW_ENABLED", "true"}});
    auto gw = h.start(h.deps());
    h.discord.add_thread("TR", "F1", "レビュー ラウンド:2");
    h.discord.add_starter("TR", "本文");
    gw->handle_dispatch("THREAD_CREATE", thread_create("TR", "F1", "レビュー ラウンド:2", {h.tag_id("議論")}));
    h.settle(*gw);
    CHECK(flow::has_paper_review("TR"));
    const auto before = h.discord.post_count();

    // reply 投稿 (message_reference あり) は宛先によらず無視。
    Json reply = message("r1", "TR", "開始");
    reply["message_reference"] = Json{{"message_id", "x"}};
    gw->handle_dispatch("MESSAGE_CREATE", reply);
    // bot / webhook の投稿も無視。
    gw->handle_dispatch("MESSAGE_CREATE", message("r2", "TR", "開始", "bot", true));
    Json hook = message("r3", "TR", "開始", "wh");
    hook["webhook_id"] = "wh";
    gw->handle_dispatch("MESSAGE_CREATE", hook);
    // bot 自身・他 bot の ✅ は承認にしない。
    gw->handle_dispatch("MESSAGE_REACTION_ADD", Json{{"user_id", "bot"}, {"channel_id", "TR"}, {"message_id", "m1"}, {"guild_id", "G1"},
                                                     {"emoji", Json{{"name", "✅"}}}});
    gw->handle_dispatch("MESSAGE_REACTION_ADD", Json{{"user_id", "b2"}, {"channel_id", "TR"}, {"message_id", "m1"}, {"guild_id", "G1"},
                                                     {"member", Json{{"user", Json{{"id", "b2"}, {"bot", true}}}}}, {"emoji", Json{{"name", "✅"}}}});
    h.settle(*gw);
    CHECK_EQ(h.discord.post_count(), before);
    CHECK_EQ(h.dispatch_count(), std::size_t{0});

    // 通常の返信 (相づち) → ☑️ を付けるがペーパーは書き換えない。
    gw->handle_dispatch("MESSAGE_CREATE", message("r4", "TR", "はい"));
    h.settle(*gw);
    const auto reactions = h.discord.snapshot(h.discord.reactions);
    CHECK(!reactions.empty() && str::starts_with(reactions.back(), "TR/r4/"));
    CHECK_EQ(h.discord.post_count(), before);

    // 人の ✅ で承認 → 議論。
    gw->handle_dispatch("MESSAGE_REACTION_ADD", Json{{"user_id", "u1"}, {"channel_id", "TR"}, {"message_id", "m1"}, {"guild_id", "G1"},
                                                     {"member", Json{{"user", Json{{"id", "u1"}, {"bot", false}}}}}, {"emoji", Json{{"name", "✅"}}}});
    h.settle(*gw);
    CHECK_EQ(h.dispatch_count(), std::size_t{1});
    CHECK(!flow::has_paper_review("TR"));
    // 承認後の 👍 はスコアリングへ (onReaction)
    gw->handle_dispatch("MESSAGE_REACTION_ADD", Json{{"user_id", "u1"}, {"channel_id", "TR"}, {"message_id", "m9"}, {"guild_id", "G1"},
                                                     {"emoji", Json{{"name", "👍"}}}});
    h.settle(*gw);
    CHECK_EQ(h.reactions_seen.size(), std::size_t{1});
    CHECK_EQ(h.reactions_seen[0].emoji, std::string("👍"));
}

TEST_CASE("壁打ち: スレッドへの返信を相手発話として取り込み、webhook で応答する") {
    GwHarness h;
    h.llm = std::make_shared<w5u::FakeLLM>(std::string("その主張の前提は何か。"));
    auto gw = h.start(h.deps());
    h.discord.add_thread("TP", "F1", "壁打ちテーマ");
    h.discord.add_starter("TP", "本文");
    gw->handle_dispatch("THREAD_CREATE", thread_create("TP", "F1", "壁打ちテーマ", {"tag-spar"}));
    h.settle(*gw);
    CHECK(flow::has_sparring_session("TP"));
    gw->handle_dispatch("MESSAGE_CREATE", message("p1", "TP", "周回は長いほうが良い"));
    h.settle(*gw);
    CHECK(!h.discord.snapshot(h.discord.webhooks).empty());
}

TEST_CASE("bot メンションは調整指示として返信する (空の指示は KG を開かずに案内)・管理外では無視") {
    GwHarness h;
    auto gw = h.start(h.deps());
    h.discord.add_thread("TM");
    Json msg = message("d1", "TM", "<@bot>");
    msg["mentions"] = Json::array({Json{{"id", "bot"}, {"username", "di-bot"}}});
    gw->handle_dispatch("MESSAGE_CREATE", msg);
    h.discord.add_thread("TX", "F9");
    Json outside = message("d2", "TX", "<@bot>");
    outside["mentions"] = msg["mentions"];
    gw->handle_dispatch("MESSAGE_CREATE", outside);
    h.settle(*gw);
    const auto posts = h.discord.snapshot(h.discord.posts);
    CHECK_EQ(posts.size(), std::size_t{1});
    CHECK_EQ(posts[0]["channel"].get<std::string>(), std::string("TM"));
    CHECK_EQ(posts[0]["message_reference"]["message_id"].get<std::string>(), std::string("d1"));
}

TEST_CASE("finalize / react: 管理外の scene・チャンネルには何もしない") {
    GwHarness h;
    auto gw = h.start(h.deps());
    h.discord.add_thread("TX", "F9");
    const auto r = gw->finalize_forum_post(std::string("discord:G1/TX"), "まとめ", "題");
    CHECK(!r.closed);
    CHECK_EQ(r.reason.value_or(""), std::string("outside managed forum"));
    CHECK(!gw->finalize_forum_post(std::string("web:room"), "まとめ", "題").closed);
    gw->react_to_message("TX", "m1", "👍");
    h.discord.add_thread("T1");
    gw->react_to_message("T1", "m1", "👍");
    const auto reactions = h.discord.snapshot(h.discord.reactions);
    CHECK_EQ(reactions.size(), std::size_t{1});
    CHECK(str::starts_with(reactions[0], "T1/m1/"));
}

TEST_CASE("キュー: 通常キューが満杯でも起動系 (THREAD_CREATE) は落ちない・満杯は数える") {
    GwHarness h;
    auto d = h.deps();
    d.normalThreads = 1;
    d.normalQueue = 1;
    auto gw = h.start(std::move(d));
    h.discord.add_thread("TQ");
    // 通常キューを詰まらせる (大量の MESSAGE_CREATE)
    for (int i = 0; i < 200; ++i) gw->handle_dispatch("MESSAGE_CREATE", message("q" + std::to_string(i), "TQ", "x"));
    h.discord.add_thread("T1", "F1", "起動 ラウンド:2");
    h.discord.add_starter("T1", "本文");
    gw->handle_dispatch("THREAD_CREATE", thread_create("T1", "F1", "起動 ラウンド:2", {h.tag_id("議論")}));
    h.settle(*gw);
    const auto s = gw->event_stats();
    CHECK(s.normalDropped > 0);
    CHECK_EQ(s.priorityDropped, std::uint64_t{0});
    CHECK_EQ(h.dispatch_count(), std::size_t{1});
}

TEST_CASE("E2E: 偽 Gateway → READY → THREAD_CREATE → 起動 → 結論 → lock/archive。token をログに出さない") {
    LogCapture logs;
    GwHarness h;
    w5u::FakeGateway fg;
    {
        std::lock_guard lock(h.discord.m);
        h.discord.gateway_url = fg.url();
    }
    auto d = h.deps();
    d.connect = true;
    d.gatewayOptions.rest = h.discord.options();
    d.gatewayOptions.allow_insecure_test_endpoints = true;
    d.gatewayOptions.reconnect_base = std::chrono::milliseconds(10);
    d.gatewayOptions.identify_window = std::chrono::milliseconds(0);
    d.gatewayOptions.close_grace = std::chrono::milliseconds(300);
    auto gw = start_discord_gateway(std::move(d));
    CHECK(gw != nullptr);
    auto conn = fg.next();
    CHECK(conn != nullptr);
    CHECK(conn->send(Json{{"op", 10}, {"d", Json{{"heartbeat_interval", 10'000}}}}));
    auto identify = conn->recv_op(2);
    CHECK(identify.has_value());
    CHECK_EQ((*identify)["d"]["token"].get<std::string>(), std::string(kToken));
    CHECK(conn->send(Json{{"op", 0}, {"t", "READY"}, {"s", 1},
                          {"d", Json{{"session_id", "s1"}, {"resume_gateway_url", fg.url()}, {"user", Json{{"id", "bot"}, {"username", "di-bot"}}},
                                     {"application", Json{{"id", "app"}}}}}}));
    CHECK(w5u::wait_until([&] { return gw->ready_done(); }));
    h.discord.add_thread("T1", "F1", "E2E ラウンド:2");
    h.discord.add_starter("T1", "本文");
    CHECK(conn->send(Json{{"op", 0}, {"t", "THREAD_CREATE"}, {"s", 2}, {"d", thread_create("T1", "F1", "E2E ラウンド:2", {h.tag_id("議論")})}}));
    // 同じイベントの再送 (resume) は 1 度だけ処理する。
    CHECK(conn->send(Json{{"op", 0}, {"t", "THREAD_CREATE"}, {"s", 3}, {"d", thread_create("T1", "F1", "E2E ラウンド:2", {h.tag_id("議論")})}}));
    CHECK(w5u::wait_until([&] {
        for (const auto& p : h.discord.snapshot(h.discord.patches))
            if (p["channel"] == "T1" && p["body"].value("archived", false)) return true;
        return false;
    }));
    h.settle(*gw);
    CHECK_EQ(h.dispatch_count(), std::size_t{1});
    CHECK(gw->gateway_stats().duplicates_dropped >= 1);
    gw->stop();
    CHECK(!logs.contains(kToken));
    CHECK(!logs.contains("fake-webhook-token"));
}

DI_TEST_MAIN()
