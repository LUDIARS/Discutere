#pragma once
// w5u_gateway_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
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

namespace w5u_gateway_support {

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

} // namespace w5u_gateway_support
