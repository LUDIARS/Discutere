// discord_hook/discord_api (Discord REST v10 クライアント) を偽の Discord REST サーバで確かめる:
// レート制限 (429 → 待って再試行・バケット・global・待ち上限)、権限エラー、壊れた応答、再試行の方針、token の伏せ字。
#include <atomic>
#include <mutex>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "base/url.hpp"
#include "check.hpp"
#include "discord_hook/discord_api.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::discord_hook;

namespace {
constexpr const char* kToken = "fake-bot-token-0123456789";

DiscordApiOptions options_for(const w1d::FakeServer& fake) {
    DiscordApiOptions o;
    o.token = kToken;
    o.base_url = fake.base;
    o.allow_private_network = true;
    o.max_rate_limit_wait = std::chrono::milliseconds(2'000);
    return o;
}

/** ログに token が出ないことを確かめるための sink。 */
struct LogCapture {
    std::mutex m;
    std::string all;
    int id;
    LogCapture() {
        id = log::add_sink([this](const log::Record& r) {
            std::lock_guard lock(m);
            all += r.message + "\n";
        });
    }
    ~LogCapture() { log::remove_sink(id); }
};
} // namespace

TEST_CASE("route keys and redaction") {
    CHECK_EQ(rate_limit_route_key("get", "/channels/123/messages/456"), std::string("GET /channels/123/messages/:id"));
    CHECK_EQ(rate_limit_route_key("PUT", "/channels/1/messages/2/reactions/%F0%9F%91%8D/@me"),
             std::string("PUT /channels/1/messages/:id/reactions/:reaction/@me"));
    CHECK_EQ(redact_route("POST", "/webhooks/99/secret-token?wait=true&thread_id=5"), std::string("POST /webhooks/99/:token"));
    CHECK_EQ(redact_route("POST", "/interactions/1/itok/callback"), std::string("POST /interactions/1/:token/callback"));
    CHECK_EQ(rate_limit_route_key("PATCH", "/webhooks/9/tok/messages/@original"), std::string("PATCH /webhooks/9/:token/messages/@original"));
    CHECK_EQ(parse_permissions(Json("16")), std::uint64_t(16));
    CHECK_EQ(parse_permissions(Json("99999999999999999999999")), std::uint64_t(0));
    CHECK_EQ(parse_permissions(Json("1x")), std::uint64_t(0));
}

TEST_CASE("headers, typed calls and JSON shapes") {
    w1d::FakeServer fake;
    std::mutex m;
    std::string ua, auth, reason, ctype, body;
    fake.router->get("/channels/:id", [&](http::Request& r) {
        std::lock_guard lock(m);
        ua = r.header("User-Agent").value_or("");
        auth = r.header("Authorization").value_or("");
        return http::Response::json(Json{{"id", r.param("id")}, {"type", 11}, {"guild_id", "g"}, {"parent_id", "f"}, {"name", "t"},
                                         {"available_tags", Json::array({Json{{"id", "1"}, {"name", "議論"}, {"moderated", false}, {"emoji_id", nullptr}, {"emoji_name", "🔥"}}})}});
    });
    fake.router->post("/guilds/:id/channels", [&](http::Request& r) {
        std::lock_guard lock(m);
        reason = r.header("X-Audit-Log-Reason").value_or("");
        body = r.body;
        return http::Response::json(Json{{"id", "new"}, {"type", 15}, {"name", "議論"}}, 201);
    });
    fake.router->post("/channels/:id/messages", [&](http::Request& r) {
        std::lock_guard lock(m);
        ctype = r.header("Content-Type").value_or("");
        body = r.body;
        return http::Response::json(Json{{"id", "m1"}, {"channel_id", r.param("id")}, {"author", {{"id", "b"}, {"username", "bot"}, {"bot", true}}},
                                         {"content", "x"}, {"timestamp", "2026-01-01T00:00:00Z"}, {"message_reference", {{"message_id", "p"}}}});
    });
    fake.router->put("/channels/:c/messages/:m/reactions/:e/@me", [&](http::Request& r) {
        std::lock_guard lock(m);
        body = r.param("e");
        return http::Response::empty(204);
    });
    fake.start();
    DiscordApi api(options_for(fake));
    auto ch = api.get_channel("123");
    CHECK_EQ(ua, std::string(DISCORD_USER_AGENT));
    CHECK_EQ(auth, std::string("Bot ") + kToken);
    CHECK(ch.is_thread());
    CHECK_EQ(*ch.parent_id, std::string("f"));
    CHECK_EQ(ch.available_tags.size(), std::size_t(1));
    CHECK_EQ(*ch.available_tags[0].emoji_name, std::string("🔥"));
    CHECK(!ch.available_tags[0].emoji_id.has_value());

    auto created = api.create_guild_channel("g", Json{{"name", "議論"}, {"type", 15}}, std::string("理由 あり"));
    CHECK_EQ(created.type, channel_type::GuildForum);
    CHECK_EQ(reason, url::encode_component("理由 あり"));

    auto msg = api.create_message("c1", Json{{"content", "hi"}}, {FileUpload{"a.json", "{\"k\":1}", "application/json"}});
    CHECK_EQ(msg.id, std::string("m1"));
    CHECK(msg.author.bot);
    CHECK_EQ(*msg.message_reference->message_id, std::string("p"));
    CHECK(str::starts_with(ctype, "multipart/form-data; boundary="));
    CHECK(str::contains(body, "name=\"payload_json\""));
    CHECK(str::contains(body, "\"attachments\":[{\"id\":0,\"filename\":\"a.json\"}]"));
    CHECK(str::contains(body, "name=\"files[0]\"; filename=\"a.json\""));
    CHECK(str::contains(body, "{\"k\":1}"));

    api.create_reaction("c1", "m1", "👍");
    CHECK_EQ(body, std::string("👍"));  // URL エンコードされて届き、サーバ側で戻る
}

TEST_CASE("429 with retry_after waits and retries; bucket headers are respected") {
    w1d::FakeServer fake;
    std::atomic<int> calls{0};
    std::atomic<long long> first_at{0}, second_at{0};
    fake.router->get("/channels/:id", [&](http::Request& r) {
        const int n = ++calls;
        if (n == 1) {
            first_at = di::time::steady_ms();
            auto res = http::Response::json(Json{{"message", "You are being rate limited."}, {"retry_after", 0.3}, {"global", false}}, 429);
            res.headers.set("X-RateLimit-Bucket", "abc");
            res.headers.set("X-RateLimit-Remaining", "0");
            res.headers.set("X-RateLimit-Reset-After", "0.3");
            return res;
        }
        second_at = di::time::steady_ms();
        return http::Response::json(Json{{"id", r.param("id")}, {"type", 0}});
    });
    fake.start();
    LogCapture logs;
    DiscordApi api(options_for(fake));
    auto ch = api.get_channel("1");
    CHECK_EQ(ch.id, std::string("1"));
    CHECK_EQ(calls.load(), 2);
    CHECK(second_at - first_at >= 280);
    CHECK(!str::contains(logs.all, kToken));

    // 残り 0 / Reset-After の間は送らずに待つ
    w1d::FakeServer fake2;
    std::atomic<int> n2{0};
    std::atomic<long long> t1{0}, t2{0};
    fake2.router->get("/guilds/:id", [&](http::Request&) {
        const int n = ++n2;
        (n == 1 ? t1 : t2) = di::time::steady_ms();
        auto res = http::Response::json(Json{{"id", "g"}, {"name", "G"}});
        res.headers.set("X-RateLimit-Bucket", "gb");
        res.headers.set("X-RateLimit-Remaining", n == 1 ? "0" : "5");
        res.headers.set("X-RateLimit-Reset-After", "0.25");
        return res;
    });
    fake2.start();
    DiscordApi api2(options_for(fake2));
    api2.get_guild("g");
    api2.get_guild("g");
    CHECK_EQ(n2.load(), 2);
    CHECK(t2 - t1 >= 230);
}

TEST_CASE("global 429 and over-limit waits fail fast") {
    w1d::FakeServer fake;
    std::atomic<int> calls{0};
    fake.router->get("/channels/:id", [&](http::Request& r) {
        ++calls;
        if (r.param("id") == "long") return http::Response::json(Json{{"retry_after", 60.0}, {"global", false}}, 429);
        auto res = http::Response::json(Json{{"retry_after", 0.1}, {"global", true}}, 429);
        return res;
    });
    fake.start();
    DiscordApi api(options_for(fake));
    const auto t0 = di::time::steady_ms();
    try {
        api.get_channel("long");
        CHECK(false);
    } catch (const DiscordApiError& e) {
        CHECK(e.rate_limited());
        CHECK_EQ(e.status(), 429);
    }
    CHECK(di::time::steady_ms() - t0 < 1'500);  // 60 秒は待たない
    // global は max_retries 回まで待って再試行し、尽きたら失敗
    calls = 0;
    try {
        api.get_channel("g");
        CHECK(false);
    } catch (const DiscordApiError& e) {
        CHECK(e.rate_limited());
    }
    CHECK_EQ(calls.load(), 4);  // 初回 + 再試行 3 回
}

TEST_CASE("permission errors, broken responses, 5xx retry policy, token never in errors") {
    w1d::FakeServer fake;
    std::atomic<int> get5xx{0}, post5xx{0};
    fake.router->get("/channels/:id", [&](http::Request& r) {
        const std::string id = r.param("id");
        if (id == "forbidden") return http::Response::json(Json{{"message", "Missing Permissions"}, {"code", 50013}}, 403);
        if (id == "broken") return http::Response::text("{not json");
        if (id == "shape") return http::Response::json(Json{{"no_id", true}});
        if (id == "huge") return http::Response::text(std::string((8u << 20) + 1, ' '));
        if (id == "flaky") {
            if (++get5xx < 3) return http::Response::text("bad gateway", 502);
            return http::Response::json(Json{{"id", "flaky"}, {"type", 0}});
        }
        return http::Response::text("nope", 404);
    });
    fake.router->post("/channels/:id/messages", [&](http::Request&) {
        ++post5xx;
        return http::Response::text("oops", 500);
    });
    fake.router->post("/webhooks/:id/:token", [](http::Request&) { return http::Response::json(Json{{"message", "Unknown Webhook"}, {"code", 10015}}, 404); });
    fake.start();
    LogCapture logs;
    DiscordApi api(options_for(fake));
    try {
        api.get_channel("forbidden");
        CHECK(false);
    } catch (const DiscordApiError& e) {
        CHECK_EQ(e.status(), 403);
        CHECK_EQ(e.code(), 50013);
        CHECK(!e.rate_limited());
        CHECK(!str::contains(e.what(), kToken));
    }
    CHECK_THROWS(api.get_channel("broken"));
    CHECK_THROWS(api.get_channel("shape"));
    CHECK_THROWS(api.get_channel("huge"));  // 応答上限 +1
    CHECK_EQ(api.get_channel("flaky").id, std::string("flaky"));
    CHECK_EQ(get5xx.load(), 3);
    try {
        api.create_message("c", Json{{"content", "x"}});
        CHECK(false);
    } catch (const DiscordApiError& e) {
        CHECK_EQ(e.status(), 500);
    }
    CHECK_EQ(post5xx.load(), 1);  // POST は再試行しない (二重投稿を避ける)
    try {
        api.execute_webhook("99", "very-secret-webhook-token", Json{{"content", "x"}});
        CHECK(false);
    } catch (const DiscordApiError& e) {
        CHECK(!str::contains(e.what(), "very-secret-webhook-token"));
        CHECK_EQ(e.route(), std::string("POST /webhooks/99/:token"));
        CHECK_EQ(e.code(), 10015);
    }
    CHECK(!str::contains(logs.all, kToken));
}

TEST_CASE("unreachable server and shutdown") {
    DiscordApiOptions o;
    o.token = kToken;
    o.base_url = "http://127.0.0.1:1";
    o.allow_private_network = true;
    o.max_retries = 1;
    DiscordApi api(o);
    try {
        api.get_channel("1");
        CHECK(false);
    } catch (const DiscordApiError& e) {
        CHECK_EQ(e.status(), 0);
        CHECK(!str::contains(e.what(), kToken));
    }
    // 本番の既定 (allow_private_network=false) は loopback へ出ない
    DiscordApiOptions d;
    d.token = kToken;
    d.base_url = "http://127.0.0.1:1";
    d.max_retries = 0;
    DiscordApi api2(d);
    try {
        api2.get_channel("1");
        CHECK(false);
    } catch (const DiscordApiError& e) {
        CHECK(str::contains(e.what(), "blocked"));
    }
    api2.shutdown();
    CHECK_THROWS(api2.get_channel("1"));
}

DI_TEST_MAIN()
