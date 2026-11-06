#pragma once
// w5u_discord_live_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// w5-U: flow/discord-live.ts の移植の検証。偽の Discord REST (loopback) と偽の LLM / 偽の dispatch で:
//   - golden: Node の discord-live がスレッドへ投稿する本文の列 (学習の Core 未設定 / レビューゲートの準備〜承認) と一致
//   - 発話の webhook 投稿 (persona 名)・投票の 🏆/👍 と集計・結論の投稿と onConcluded
//   - ペーパーレビュー: 準備中の返信の溜め置き・同じスレッドの二重準備の拒否・✅ 承認・破棄・やり直し
//   - 壁打ちの継続 (返信 → 応答 → /終了)
//   - 境界: 空返信・巨大な返信・1900 文字の切り詰め・背景キュー満杯
#include "check.hpp"
#include "w5u_support.hpp"

#include <thread>

#include "base/strings.hpp"
#include "flow/discord_flow_runs.hpp"
#include "flow/discord_live.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/paper_review_session.hpp"
#include "flow/persona_display.hpp"
#include "flow/user_voices/preparation.hpp"

using namespace di;
using namespace di::flow;
using w5u::Json;

namespace w5u_discord_live_support {

user_voices::VoiceRequestPort fake_voice_request() {
    user_voices::VoiceRequestPort p;
    p.submit = [](const std::string& id, const std::string&) { return user_voices::VoiceRequestReceipt{true, id}; };
    p.status = [](const std::string& id) { return user_voices::VoiceRequestReceipt{true, id}; };
    return p;
}

struct Harness {
    w5u::FakeDiscord discord;
    std::shared_ptr<discord_hook::DiscordApi> api = std::make_shared<discord_hook::DiscordApi>(discord.options());
    std::shared_ptr<w5u::FakeLLM> llm = std::make_shared<w5u::FakeLLM>();
    Executor ex{"w5u-bg", 2, 64};
    std::mutex m;
    std::vector<DispatchInput> dispatched;
    std::vector<DispatchDeps> dispatched_deps;
    std::vector<std::string> concluded;  // scene|title|summary

    FlowDiscordDeps deps(const std::string& games) {
        FlowDiscordDeps d;
        d.api = api;
        d.llm = llm;
        d.gamesDir = games;
        d.voiceRequest = fake_voice_request();
        d.submitBackground = [this](std::function<void()> f) { return ex.submit(std::move(f)); };
        d.prepareUserVoices = [](const user_voices::PrepareUserVoicesArgs&) { return user_voices::PreparedUserVoices{}; };
        d.expandNotionLinks = [](const std::string&) { return std::string(); };
        // 名前付き返り値の最適化をそのまま使うと、Praesidium の静的解析が std::function の内部ポインタを
        // 「局所変数のアドレスを返した」と誤検知する。返す値は明示的な複製にする。
        return FlowDiscordDeps(d);
    }
    FlowLiveHooks hooks() {
        FlowLiveHooks h;
        h.onConcluded = [this](const std::string& scene, const std::string& title, const std::string& summary) {
            std::lock_guard lock(m);
            concluded.push_back(scene + "|" + title + "|" + summary);
        };
        return h;
    }
    /** 2 人の発話 + 投票 + 結論を返す偽の議論 (session 行を started にする)。 */
    std::function<DispatchResult(const DispatchInput&, const DispatchDeps&)> fake_discussion(bool concluded_flag = true) {
        return [this, concluded_flag](const DispatchInput& in, const DispatchDeps& d) {
            {
                std::lock_guard lock(m);
                dispatched.push_back(in);
                dispatched_deps.push_back(d);
            }
            if (d.sessionId) {
                NewPaper p;
                p.sessionId = *d.sessionId;
                p.theme = in.theme;
                p.tags = in.tags;
                persist_paper(p, in.flow);
            }
            FlowUtteranceRecord a;
            a.id = "u-a";
            a.personaName = "相馬";
            a.role = "debater";
            a.stance = "pro";
            a.text = "反証: それは違う。別の見方もある！";
            FlowUtteranceRecord b = a;
            b.id = "u-b";
            b.personaName = "成瀬";
            b.stance = "con";
            b.text = "賛成です";
            if (d.onUtterance) {
                d.onUtterance(a);
                d.onUtterance(b);
            }
            if (d.onVote) {
                VoteEvent e;
                e.round = 1;
                e.kind = "votes";
                e.tally = {{"u-a", 2}, {"u-b", 1}};
                e.winner = "u-a";
                d.onVote(e);
            }
            DispatchResult r;
            r.kind = in.flow;
            r.flow = in.flow;
            FlowDirectorResult res;
            res.concluded = concluded_flag;
            res.conclusion = "周回は短くする";
            r.result = res;
            return r;
        };
    }
};

StartForumFlowInput input(const std::string& thread, const std::string& flow, std::vector<FlowTag> tags = {}) {
    StartForumFlowInput in;
    in.guildId = "G1";
    in.threadId = thread;
    in.theme = "周回の面白さ";
    in.flow = flow;
    in.tags = std::move(tags);
    return in;
}

std::vector<std::string> contents(const Json& posts) {
    std::vector<std::string> out;
    for (const auto& p : posts) out.push_back(p["content"].get<std::string>());
    return out;
}

} // namespace w5u_discord_live_support









/** dispatch の壁打ち: TS の parseFlowKind は学習・壁打ちのラベルを持たないため dispatchFlow 経由では起動できない
 *  (TS も同じく「議論タイプが未指定または不正」になる)。discord-live の壁打ち経路は session を直接作る偽 dispatch で確かめる。 */
std::function<DispatchResult(const DispatchInput&, const DispatchDeps&)> sparring_dispatch() {
    return [](const DispatchInput& in, const DispatchDeps& d) {
        SparringDeps sd;
        sd.llm = d.llm;
        sd.onUtterance = d.onUtterance;
        sd.gamesDir = d.gamesDir;
        sd.warn = d.warn;
        auto session = std::make_shared<SparringSession>(in.theme, in.tags, sd);
        session->start();
        DispatchResult r;
        r.kind = "sparring";
        r.flow = "sparring";
        r.session = session;
        return r;
    };
}
