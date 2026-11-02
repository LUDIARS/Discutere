// discussion/director-live.ts の移植の検証 (TS テスト無し)。偽の Discord REST サーバ + mock LLM で
// 開始の投稿・webhook (スレッドは親の webhook + thread_id)・続行の問い・ボタン (チャンネル照合・期限切れ)・無応答の停止。
#include "check.hpp"

#include <atomic>
#include <future>
#include <thread>

#include "base/strings.hpp"
#include "discord_hook/poster.hpp"
#include "discussion/director_live.hpp"
#include "persona_engine/llm/mock.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::discussion;
using namespace di::discord_hook;
using namespace std::chrono_literals;

namespace {

struct FakeDiscord {
    w1d::FakeServer fake;
    std::mutex m;
    std::vector<Json> webhook_posts;
    std::vector<std::string> webhook_queries;
    std::vector<std::string> webhook_channels;
    std::vector<std::pair<std::string, Json>> messages;
    std::vector<std::pair<std::string, Json>> callbacks;
    std::function<void(const std::string& channel, const Json& body)> on_message;

    FakeDiscord() {
        auto& r = *fake.router;
        r.get("/channels/:id", [](http::Request& q) {
            const auto id = q.param("id");
            if (id == "thread") return http::Response::json(Json{{"id", "thread"}, {"type", 11}, {"parent_id", "forum"}});
            if (id == "forum") return http::Response::json(Json{{"id", "forum"}, {"type", 15}});
            if (id == "missing") return http::Response::json(Json{{"message", "Unknown Channel"}, {"code", 10003}}, 404);
            return http::Response::json(Json{{"id", id}, {"type", 0}});
        });
        r.get("/channels/:id/webhooks", [this](http::Request& q) {
            std::lock_guard lock(m);
            webhook_channels.push_back(q.param("id"));
            return http::Response::json(Json::array());
        });
        r.post("/channels/:id/webhooks", [](http::Request& q) {
            return http::Response::json(Json{{"id", "w-" + q.param("id")}, {"name", "Discutere議論"}, {"token", "fake-webhook-token"}});
        });
        r.post("/webhooks/:id/:token", [this](http::Request& q) {
            std::lock_guard lock(m);
            CHECK(!q.header("Authorization"));
            webhook_posts.push_back(Json::parse(q.body));
            webhook_queries.push_back(q.query_string);
            return http::Response::json(Json{{"id", "wm"}, {"channel_id", "x"}});
        });
        r.post("/channels/:id/messages", [this](http::Request& q) {
            const Json body = Json::parse(q.body);
            std::function<void(const std::string&, const Json&)> cb;
            {
                std::lock_guard lock(m);
                messages.emplace_back(q.param("id"), body);
                cb = on_message;
            }
            if (cb) std::thread([cb, ch = q.param("id"), body] { std::this_thread::sleep_for(30ms); cb(ch, body); }).detach();
            return http::Response::json(Json{{"id", "m1"}, {"channel_id", q.param("id")}});
        });
        r.post("/interactions/:id/:token/callback", [this](http::Request& q) {
            std::lock_guard lock(m);
            CHECK(!q.header("Authorization"));
            callbacks.emplace_back(q.param("id"), Json::parse(q.body));
            return http::Response::empty(204);
        });
        fake.start();
    }

    std::shared_ptr<DiscordApi> api() {
        DiscordApiOptions o;
        o.token = "fake-bot-token";
        o.base_url = fake.base;
        o.allow_private_network = true;
        return std::make_shared<DiscordApi>(o);
    }
};

DebateRunnerOptions options(std::shared_ptr<DiscordApi> api, double continue_timeout_ms = 3000) {
    DebateRunnerOptions o;
    o.api = std::move(api);
    o.discussion.expectedUtterances = 4;
    o.discussion.keymanCount = 1;
    o.discussion.facilitatorModel = "claude-f";
    o.discussion.keymanModel = "claude-k";
    o.discussion.opinionModels = {{"claude-o", 1}};
    o.discussion.minTotal = 3;
    o.discussion.turnDelayMs = 0;
    o.discussion.continueTimeoutMs = continue_timeout_ms;
    o.discussion.maxRounds = 3;
    o.llm = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{}, "発言です");
    o.rng = [] { return 0.3; };
    return o;
}

std::string token_of(const Json& msg, int button) {
    return msg["components"][0]["components"][button]["custom_id"].get<std::string>();
}

} // namespace

TEST_CASE("director-live: 続行の問いのメッセージ形") {
    const auto m = build_continue_message(20, {1, 20}, "123-0");
    CHECK_EQ(m.dump(),
             std::string(R"j({"content":"🗳️ 想定発話数 (20) に到達しました (ラウンド1 / 累計20発話)。議論を続けますか?\n続ける場合、司会を除くメンバーを半数入れ替えます。","components":[{"type":1,"components":[{"type":2,"custom_id":"debate:cont:123-0","label":"続ける","style":3},{"type":2,"custom_id":"debate:stop:123-0","label":"止める","style":4}]}]})j"));
}

TEST_CASE("director-live: スレッドで議論 → 続ける → 別チャンネルのボタンは無視 → 止める") {
    FakeDiscord d;
    clear_webhook_cache();
    auto runner = create_debate_runner(options(d.api()));
    std::atomic<int> asked{0};
    d.on_message = [&](const std::string& ch, const Json& body) {
        const int n = ++asked;
        const auto cont = token_of(body, 0);
        const auto stop = token_of(body, 1);
        // コピーされたトークンは別スレッドの議論を操作できない
        CHECK(runner->handle_button({"i-other", "tok", stop, std::string("other-thread")}));
        if (n == 1)
            CHECK(runner->handle_button({"i1", "itok", cont, ch}));
        else
            CHECK(runner->handle_button({"i2", "itok", stop, ch}));
    };
    runner->start("thread", "テーマ");
    CHECK_EQ(runner->active_count(), 0);
    CHECK_EQ(asked.load(), 2);
    std::lock_guard lock(d.m);
    // スレッドは親 (forum) の webhook に thread_id を付けて投稿する
    for (const auto& c : d.webhook_channels) CHECK_EQ(c, std::string("forum"));
    CHECK_EQ(d.webhook_posts.front()["username"].get<std::string>(), std::string("司会"));
    CHECK_EQ(d.webhook_posts.front()["content"].get<std::string>(), std::string("📣 議論を始めます: 「テーマ」"));
    for (const auto& q : d.webhook_queries) CHECK(str::contains(q, "thread_id=thread"));
    CHECK(str::starts_with(d.webhook_posts.back()["content"].get<std::string>(), "🧷 まとめ: "));
    CHECK_EQ(d.messages.size(), std::size_t(2));
    CHECK_EQ(d.messages[0].first, std::string("thread"));
    CHECK_EQ(d.callbacks.size(), std::size_t(2));
    CHECK_EQ(d.callbacks[0].second.dump(), std::string(R"j({"type":7,"data":{"content":"✅ 続行します (メンバーを半数入れ替えます)","components":[]}})j"));
    CHECK_EQ(d.callbacks[1].second["data"]["content"].get<std::string>(), std::string("🛑 ここで議論を締めます"));
}

TEST_CASE("director-live: 無応答はタイムアウトで停止・期限切れのボタン・関係ない custom_id") {
    FakeDiscord d;
    clear_webhook_cache();
    auto runner = create_debate_runner(options(d.api(), 200));
    runner->start("c1", "t");
    std::string token;
    {
        std::lock_guard lock(d.m);
        CHECK_EQ(d.messages.size(), std::size_t(1));
        token = token_of(d.messages[0].second, 0);
        CHECK(str::starts_with(d.webhook_posts.back()["content"].get<std::string>(), "🧷 まとめ: "));
    }
    CHECK(runner->handle_button({"i3", "itok", token, std::string("c1")}));
    CHECK(runner->handle_button({"i4", "itok", "debate", std::string("c1")}) == false);
    CHECK(runner->handle_button({"i5", "itok", "debate:cont", std::string("c1")}));
    CHECK(!runner->handle_button({"i6", "itok", "flow-pick:1", std::string("c1")}));
    std::lock_guard lock(d.m);
    CHECK_EQ(d.callbacks.size(), std::size_t(2));
    CHECK_EQ(d.callbacks[0].second.dump(), std::string(R"({"type":4,"data":{"content":"この問いかけは期限切れです。","flags":64}})"));
}

TEST_CASE("director-live: 送れないチャンネル (フォーラム) は問わずに停止・存在しないチャンネルは warn で終わる") {
    FakeDiscord d;
    clear_webhook_cache();
    auto runner = create_debate_runner(options(d.api()));
    runner->start("forum", "t");
    {
        std::lock_guard lock(d.m);
        CHECK(d.messages.empty());
        CHECK(!d.webhook_posts.empty());
    }
    runner->start("missing", "t");  // webhook 確保に失敗 → 例外を外に出さない
    CHECK_EQ(runner->active_count(), 0);
}

DI_TEST_MAIN()
