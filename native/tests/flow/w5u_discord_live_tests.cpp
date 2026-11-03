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

namespace {

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
        return d;
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

} // namespace

TEST_CASE("golden: 学習フローは Core 未設定なら起動できない旨を出す (Node と同じ本文)") {
    w5u::TempEnv env;
    reset_flow_live_for_test();
    Harness h;
    const auto fx = w5u::load_fixture("discord_live.json");
    start_forum_flow(input("TL", "learning"), h.deps(env.games()));
    CHECK(h.discord.post_contents("TL") == contents(fx["learningNoCore"]));
}

TEST_CASE("golden: レビューゲート — 準備・草案・声の準備・案内・戻す・相づち・調整・受付確認・開始 (Node と同じ投稿列)") {
    w5u::TempEnv env({{"DISCUTERE_FLOW_PAPER_REVIEW_ENABLED", "true"}});
    reset_flow_live_for_test();
    Harness h;
    auto deps = h.deps(env.games());
    deps.dispatch = h.fake_discussion();
    const auto fx = w5u::load_fixture("discord_live.json")["review"];
    start_forum_flow(input("TR", "discussion", {"機密"}), deps);
    std::vector<std::size_t> marks{h.discord.post_count()};
    for (const char* text : {"戻す", "はい", "メカニクスにガチャを追加", "受付確認"}) {
        CHECK(handle_paper_review_reply("TR", "G1", text, deps));
        marks.push_back(h.discord.post_count());
    }
    CHECK_EQ(has_paper_review("TR"), fx["hadReview"].get<bool>());
    CHECK(handle_paper_review_reply("TR", "G1", "開始", deps));
    CHECK(w5u::drain(h.ex));
    const auto expected = contents(fx["posts"]);
    auto actual = h.discord.post_contents("TR");
    CHECK(actual.size() >= expected.size());
    actual.resize(expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) CHECK_EQ(actual[i], expected[i]);
    for (std::size_t i = 0; i < marks.size(); ++i) CHECK_EQ(marks[i], fx["marks"][i].get<std::size_t>());
    // 承認後は議論 (偽) が走り、ペーパーは started になってレビュー待ちではなくなる。
    CHECK_EQ(h.dispatched.size(), std::size_t{1});
    CHECK(h.dispatched_deps[0].paperOverride.has_value());
    CHECK_EQ(h.dispatched_deps[0].sessionId.value_or(""), forum_session_id("TR"));
    CHECK(!has_paper_review("TR"));
    CHECK(!is_discussion_running("TR"));
    // approval_guide は既定 (タイムアウトなし) で Node と同じ。
    CHECK_EQ(approval_guide(), w5u::load_fixture("discord_live.json")["approvalGuideDefault"].get<std::string>());
}

TEST_CASE("approval_guide: timeoutMs があれば分 / 秒で併記する (Math.round)") {
    {
        w5u::TempEnv env({{"DISCUTERE_FLOW_PAPER_REVIEW_TIMEOUT_MS", "120000"}});
        CHECK(str::ends_with(approval_guide(), "\n(2 分無操作なら草案のまま自動で始めます。ただし外部の声の選択待ち・受付待ちでは自動開始しません)"));
    }
    {
        w5u::TempEnv env({{"DISCUTERE_FLOW_PAPER_REVIEW_TIMEOUT_MS", "29000"}});
        CHECK(str::ends_with(approval_guide(), "\n(29 秒無操作なら草案のまま自動で始めます。ただし外部の声の選択待ち・受付待ちでは自動開始しません)"));
    }
    {
        w5u::TempEnv env({{"DISCUTERE_FLOW_PAPER_REVIEW_TIMEOUT_MS", "90000"}});
        CHECK(approval_guide().find("(2 分無操作") != std::string::npos);  // 1.5 → 2 (Math.round)
    }
}

TEST_CASE("議論 (レビューなし): 発話を persona 名で webhook 投稿・投票の 🏆/👍・集計・結論・onConcluded") {
    w5u::TempEnv env;
    reset_flow_live_for_test();
    Harness h;
    h.discord.add_thread("TD");
    auto deps = h.deps(env.games());
    deps.dispatch = h.fake_discussion();
    deps.listExternalVoices = [](const std::vector<std::string>&, int) { return std::vector<ContextVoice>{ContextVoice{"声", "steam", {}}}; };
    start_forum_flow(input("TD", "discussion"), deps, h.hooks());
    const auto posts = h.discord.post_contents("TD");
    CHECK_EQ(posts.size(), std::size_t{3});
    CHECK_EQ(posts[0], std::string("🗣️ **企画/議論**のを始めます: 「周回の面白さ」"));
    CHECK(str::starts_with(posts[1], "**🗳️ ラウンド 1 投票結果**\n"));
    CHECK_EQ(posts[2], std::string("✅ **結論**\n周回は短くする"));
    const auto hooks = h.discord.snapshot(h.discord.webhooks);
    CHECK_EQ(hooks.size(), std::size_t{2});
    CHECK_EQ(hooks[0]["username"].get<std::string>(), compose_display_name(DisplayNameInput{"相馬", std::string("pro"), std::string("debater"), {}}));
    CHECK_EQ(hooks[0]["content"].get<std::string>(), std::string("それは違う。\n別の見方もある！"));  // humanize
    CHECK_EQ(hooks[0]["thread_id"].get<std::string>(), std::string("TD"));                      // 親の webhook + thread_id
    const auto reactions = h.discord.snapshot(h.discord.reactions);
    CHECK_EQ(reactions.size(), std::size_t{2});
    CHECK(str::starts_with(reactions[0], "TD/w2/"));  // winner (最初の発話 = 2 番目のメッセージ) に 🏆
    CHECK(str::starts_with(reactions[1], "TD/w3/"));  // 得票ありに 👍
    CHECK(str::ends_with(reactions[0], "/" + std::string("%F0%9F%8F%86")) || str::ends_with(reactions[0], "/🏆"));
    CHECK_EQ(h.concluded.size(), std::size_t{1});
    CHECK_EQ(h.concluded[0], std::string("discord:G1/TD|周回の面白さ|周回は短くする"));
    CHECK(!is_discussion_running("TD"));
    // bot の投稿は Authorization: Bot <token> (token はログに出さない)
    for (const auto& a : h.discord.snapshot(h.discord.auth_headers)) CHECK_EQ(a, std::string("Bot fake-bot-token-not-real"));
}

TEST_CASE("議論 (レビューなし): 声が引けないときは草案レビューに回る・未収束は (明確な結論には至りませんでした)") {
    w5u::TempEnv env;
    reset_flow_live_for_test();
    Harness h;
    auto deps = h.deps(env.games());
    deps.dispatch = h.fake_discussion(false);
    start_forum_flow(input("TV", "improvement"), deps, h.hooks());
    CHECK(has_paper_review("TV"));  // 声が無い → レビュー
    CHECK(handle_paper_review_approval("TV", deps, h.hooks(), std::string("G1")));
    CHECK(w5u::drain(h.ex));
    const auto posts = h.discord.post_contents("TV");
    CHECK_EQ(posts.back(), std::string("✅ **結論**\n(明確な結論には至りませんでした)"));
    CHECK_EQ(h.dispatched[0].flow, std::string("improvement"));
    CHECK_EQ(h.concluded.size(), std::size_t{1});
}

TEST_CASE("準備中の返信は溜めて草案の後に届いた順に処理する・同じスレッドで準備を二重に始めない") {
    w5u::TempEnv env({{"DISCUTERE_FLOW_PAPER_REVIEW_ENABLED", "true"}});
    reset_flow_live_for_test();
    Harness h;
    auto deps = h.deps(env.games());
    deps.dispatch = h.fake_discussion();
    std::atomic<bool> second_rejected{false};
    deps.prepareUserVoices = [&](const user_voices::PrepareUserVoicesArgs&) {
        // 準備中: 返信は溜まり (true)、承認は草案前なので false、同じスレッドの再起動は何も投稿しない。
        CHECK(is_preparing("TB"));
        CHECK(handle_paper_review_reply("TB", "G1", "開始", deps));
        CHECK(!handle_paper_review_approval("TB", deps));
        const auto before = h.discord.post_count();
        start_forum_flow(input("TB", "discussion"), deps);
        second_rejected = h.discord.post_count() == before;
        return user_voices::PreparedUserVoices{};
    };
    start_forum_flow(input("TB", "discussion"), deps);
    CHECK(second_rejected.load());
    CHECK(w5u::drain(h.ex));
    const auto posts = h.discord.post_contents("TB");
    // 草案 → 声の準備 → 案内 の後に、溜めた「開始」で承認される。
    CHECK(posts.size() >= 5);
    CHECK(str::starts_with(posts[1], "📝 **ディスカッションペーパー"));
    CHECK(str::starts_with(posts[3], "✏️"));
    CHECK_EQ(posts[4], std::string("✅ ペーパーを承認しました。議論を始めます…"));
    CHECK_EQ(h.dispatched.size(), std::size_t{1});
    CHECK_EQ(detail::serial_queue_count(), std::size_t{0});
}

TEST_CASE("✅ 承認 / 破棄 / フロー再提案の続行") {
    w5u::TempEnv env({{"DISCUTERE_FLOW_PAPER_REVIEW_ENABLED", "true"}});
    reset_flow_live_for_test();
    Harness h;
    auto deps = h.deps(env.games());
    deps.dispatch = h.fake_discussion();
    // 承認待ちが無ければ no-op (TS paper-review.test.ts と同じ)。
    CHECK(!handle_paper_review_approval("no-such-thread", deps));

    start_forum_flow(input("TC", "discussion"), deps);
    CHECK(has_paper_review("TC"));
    // 議論 → 改善 に選び直すと今のペーパーのまま続ける。
    CHECK(continue_paper_review_with_flow("TC", "improvement", deps));
    CHECK(str::starts_with(h.discord.post_contents("TC").back(), "👌 今のペーパーのまま「改善/議論」で続けます"));
    CHECK(!continue_paper_review_with_flow("TC", "sparring", deps));
    // 破棄すると draft 行も消え、返信は横取りされない。
    CHECK(cancel_paper_review("TC"));
    CHECK(!has_paper_review("TC"));
    CHECK(!get_draft_paper(forum_session_id("TC")).has_value());
    CHECK(!handle_paper_review_reply("TC", "G1", "開始", deps));

    // メモリから消えても永続ドラフトから再開でき、✅ で承認できる。
    start_forum_flow(input("TE", "discussion"), deps);
    reset_flow_live_for_test();  // 再起動相当 (draft 行は残る)
    CHECK(has_paper_review("TE"));
    CHECK(handle_paper_review_approval("TE", deps, {}, std::string("G1")));
    CHECK(w5u::drain(h.ex));
    CHECK_EQ(h.dispatched.size(), std::size_t{1});
    CHECK_EQ(h.dispatched[0].scene.value_or(""), std::string("discord:G1/TE"));
    CHECK(!has_paper_review("TE"));
}

TEST_CASE("途中で止まった議論のやり直し: 「再開」で確定ペーパーから・進行中は案内だけ・下書きや結論済みは対象外") {
    w5u::TempEnv env;
    reset_flow_live_for_test();
    Harness h;
    auto deps = h.deps(env.games());
    deps.dispatch = h.fake_discussion();
    NewPaper p;
    p.sessionId = forum_session_id("TR2");
    p.theme = "周回の面白さ";
    p.bodyMd = "# 議題\n周回の面白さ\n";
    persist_paper(p, "discussion");
    CHECK(!handle_interrupted_discussion_reply("TR2", "G1", "こんにちは", deps));  // やり直しの語でない
    CHECK(handle_interrupted_discussion_reply("TR2", "G1", "再開", deps, h.hooks()));
    CHECK_EQ(h.dispatched.size(), std::size_t{1});
    CHECK(h.dispatched_deps[0].paperOverride && h.dispatched_deps[0].paperOverride->bodyMd);
    CHECK(str::starts_with(h.discord.post_contents("TR2")[0], "🔁 途中で止まった議論を"));
    CHECK(!handle_interrupted_discussion_reply("TR2-none", "G1", "再開", deps));  // ペーパー無し
}

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

TEST_CASE("壁打ち: 本物の dispatch は TS と同じく壁打ちを受理しない (エラーをスレッドに通知)") {
    w5u::TempEnv env;
    reset_flow_live_for_test();
    Harness h;
    start_forum_flow(input("TS0", "sparring"), h.deps(env.games()));
    CHECK(!has_sparring_session("TS0"));
    CHECK(str::starts_with(h.discord.post_contents("TS0").back(), "⚠️ フロー実行中にエラーが発生しました: "));
}

TEST_CASE("壁打ち: 起動 → 返信で応答 → /終了 で締めて onConcluded") {
    w5u::TempEnv env;
    reset_flow_live_for_test();
    Harness h;
    h.discord.add_thread("TS");
    auto deps = h.deps(env.games());
    h.llm = std::make_shared<w5u::FakeLLM>(std::string("反証: その主張には前提がある。"));
    deps.llm = h.llm;
    deps.dispatch = sparring_dispatch();
    start_forum_flow(input("TS", "sparring"), deps, h.hooks());
    CHECK(has_sparring_session("TS"));
    CHECK(str::starts_with(h.discord.post_contents("TS").back(), "🥊 **壁打ち**を始めます"));
    CHECK(handle_forum_flow_reply("TS", "G1", "周回は長いほうが面白い", deps, h.hooks()));
    CHECK(!h.discord.snapshot(h.discord.webhooks).empty());  // AI の応答は webhook で persona 名
    CHECK(handle_forum_flow_reply("TS", "G1", "/終了", deps, h.hooks()));
    CHECK(!has_sparring_session("TS"));
    CHECK(str::starts_with(h.discord.post_contents("TS").back(), "✅ **壁打ち終了**\n"));
    CHECK_EQ(h.concluded.size(), std::size_t{1});
    CHECK(str::starts_with(h.concluded[0], "discord:G1/TS|壁打ち|"));
    CHECK(!handle_forum_flow_reply("TS", "G1", "まだ?", deps));  // 終わった壁打ちは取り込まない
}

TEST_CASE("境界: 空返信・巨大な返信・1900 文字の切り詰め・背景キュー満杯") {
    w5u::TempEnv env({{"DISCUTERE_FLOW_PAPER_REVIEW_ENABLED", "true"}});
    reset_flow_live_for_test();
    Harness h;
    auto deps = h.deps(env.games());
    deps.dispatch = h.fake_discussion();
    start_forum_flow(input("TX", "discussion"), deps);
    const auto before = h.discord.post_count();
    CHECK(handle_paper_review_reply("TX", "G1", "   ", deps));  // 空返信は無視 (レビューは継続)
    CHECK_EQ(h.discord.post_count(), before);
    // 巨大な調整指示 (1MB) — LLM は失敗し、元のペーパーを保って「編集に失敗」を返す。
    CHECK(handle_paper_review_reply("TX", "G1", std::string(1u << 20, 'a'), deps));
    CHECK(has_paper_review("TX"));
    // 背景キューが満杯なら承認しても議論を始めず、レビュー待ちに戻す。
    deps.submitBackground = [](std::function<void()>) { return false; };
    CHECK(handle_paper_review_reply("TX", "G1", "開始", deps));
    CHECK(str::starts_with(h.discord.post_contents("TX").back(), "⚠️ 混雑のため議論を開始できませんでした"));
    CHECK(has_paper_review("TX"));
    CHECK(!is_discussion_running("TX"));
    CHECK_EQ(h.dispatched.size(), std::size_t{0});
    // 通知の本文は 1900 文字 (UTF-16) で切る (poster)。テーマを長くすると準備の通知が切られる。
    auto in = input("TY", "discussion");
    in.theme = std::string(5000, 'x');
    start_forum_flow(in, deps);
    for (const auto& c : h.discord.post_contents("TY")) CHECK(str::js_length(c) <= 1900);
}

TEST_CASE("API 未設定・LLM 未設定は投稿せずに warn で終わる (例外を外へ出さない)") {
    w5u::TempEnv env;
    reset_flow_live_for_test();
    FlowDiscordDeps deps;
    start_forum_flow(input("TZ", "discussion"), deps);
    CHECK(!has_paper_review("TZ"));
    CHECK(!is_preparing("TZ"));
}

DI_TEST_MAIN()
