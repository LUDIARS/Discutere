#pragma once
// w5-U のテスト共通の補助: 偽の Discord REST (loopback の http::Server)、偽の LLM、一時の flow DB と config、
// テスト用の上限つき executor。本物の Discord・LLM・Voluptas には繋がない。
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/executor.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "check.hpp"
#include "config.hpp"
#include "discord_hook/discord_api.hpp"
#include "discord_hook/forum_flow_tags.hpp"
#include "discord_hook/poster.hpp"
#include "discord_hook/system_channel.hpp"
#include "flow/db/connection.hpp"
#include "persona_engine/llm/client.hpp"
#include "w1d_support.hpp"

namespace w5u {

using di::Json;
namespace http = di::http;

inline Json load_fixture(const std::string& name) {
    auto text = di::fs::read_file(di::test::fixtures_dir() + "/w5u/" + name, 16u << 20);
    if (!text) throw di::test::Failure("fixture missing: " + name);
    return di::parse_json_or_throw(*text, {16u << 20, 2'000'000, 64});
}

/** 一時の flow DB と config (DISCUTERE_CONFIG は存在しないファイル = 既定値 + env)。 */
struct TempEnv {
    std::filesystem::path dir;
    std::vector<std::string> keys;
    explicit TempEnv(const std::map<std::string, std::string>& env = {}) {
        dir = std::filesystem::temp_directory_path() / ("di-w5u-" + di::crypto::uuid_v4());
        std::filesystem::create_directories(dir / "games");
        set("DATABASE_PATH", di::fs::from_path(dir / "flow.db"));
        set("DISCUTERE_CONFIG", di::fs::from_path(dir / "no-config.json"));
        set("DISCUTERE_FLOW_PAPER_REVIEW_ENABLED", std::nullopt);
        set("DISCUTERE_FLOW_PAPER_REVIEW_TIMEOUT_MS", std::nullopt);
        set("DISCUTERE_YOUTUBE_API_KEY", std::nullopt);
        for (const auto& [k, v] : env) set(k, v);
        di::config::reset_config_for_test();
        di::flow::db::reset_flow_db_for_test();
        di::discord_hook::clear_webhook_cache();
        di::discord_hook::clear_system_channel_cache();
    }
    void set(const std::string& k, std::optional<std::string> v) {
        keys.push_back(k);
        if (v) di::env::set_for_test(k, std::string_view(*v));
        else di::env::set_for_test(k, std::nullopt);
    }
    std::string games() const { return di::fs::from_path(dir / "games"); }
    ~TempEnv() {
        for (const auto& k : keys) di::env::set_for_test(k, std::nullopt);
        di::config::reset_config_for_test();
        di::flow::db::reset_flow_db_for_test();
        di::discord_hook::clear_webhook_cache();
        di::discord_hook::clear_system_channel_cache();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

/** 常に失敗を返す (または決まった応答を返す) 偽の LLM。呼び出し回数を数える。 */
class FakeLLM : public di::persona_engine::LLMClient {
public:
    explicit FakeLLM(std::optional<std::string> text = std::nullopt) : text_(std::move(text)) {}
    di::persona_engine::LLMResult invoke(const di::persona_engine::LLMInvokeArgs&) override {
        ++calls;
        if (text_) return di::persona_engine::LLMResult::success(*text_);
        return di::persona_engine::LLMResult::failure("golden");
    }
    std::atomic<int> calls{0};

private:
    std::optional<std::string> text_;
};

/** 上限つきの executor が空になるまで待つ。 */
inline bool drain(di::Executor& ex, std::chrono::milliseconds timeout = std::chrono::milliseconds(10'000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ex.queued() == 0 && ex.running() == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

inline Json channel_json(const std::string& id, int type, const std::string& guild, const std::optional<std::string>& parent,
                         const std::optional<std::string>& name = std::nullopt) {
    Json c{{"id", id}, {"type", type}, {"guild_id", guild}};
    if (parent) c["parent_id"] = *parent;
    if (name) c["name"] = *name;
    return c;
}

/** 偽の Discord REST。投稿・webhook・リアクション・interaction 応答・PATCH を記録する。 */
struct FakeDiscord {
    w1d::FakeServer fake;
    std::mutex m;
    std::vector<Json> posts;        // {channel, content, components?, message_reference?}
    std::vector<Json> webhooks;     // {username, content, thread_id}
    std::vector<std::string> reactions;  // "<channel>/<message>/<emoji>"
    std::vector<Json> callbacks;    // {id, body}
    std::vector<Json> edits;        // @original の PATCH 本文
    std::vector<Json> patches;      // {channel, body}
    std::vector<std::string> auth_headers;
    std::map<std::string, Json> channels;
    std::map<std::string, Json> starters;  // thread id → starter message
    std::string gateway_url;
    int commands_registered = 0;
    int message_seq = 0;

    FakeDiscord() {
        // 議論フォーラム F1 (G1)、管理外のフォーラム F9 (G1)、運用チャンネル。
        Json tags = Json::array();
        int i = 0;
        for (const auto& n : di::discord_hook::all_flow_tag_names()) tags.push_back(Json{{"id", "tag-" + std::to_string(i++)}, {"name", n}});
        Json f1 = channel_json("F1", 15, "G1", std::nullopt, "議論");
        f1["available_tags"] = tags;
        channels["F1"] = f1;
        Json f9 = channel_json("F9", 15, "G1", std::nullopt, "別フォーラム");
        f9["available_tags"] = tags;
        channels["F9"] = f9;
        channels["cat"] = channel_json("cat", 4, "G1", std::nullopt, "システム");
        channels["dl"] = channel_json("dl", 0, "G1", std::string("cat"), "データ学習依頼");
        channels["sum"] = channel_json("sum", 0, "G1", std::string("cat"), "まとめ投稿");
        auto& r = *fake.router;
        r.get("/users/@me", [](http::Request&) { return http::Response::json(Json{{"id", "bot"}, {"username", "di-bot"}, {"bot", true}}); });
        r.get("/guilds/:id", [](http::Request& q) {
            return http::Response::json(Json{{"id", q.param("id")}, {"name", "g"}, {"owner_id", "bot"}, {"roles", Json::array()}});
        });
        r.get("/guilds/:id/channels", [this](http::Request& q) {
            std::lock_guard lock(m);
            Json list = Json::array();
            for (const auto& [id, c] : channels)
                if (c.value("guild_id", "") == q.param("id") && !c.contains("thread_metadata") && c.value("type", 0) != 11) list.push_back(c);
            return http::Response::json(list);
        });
        r.get("/guilds/:id/threads/active", [this](http::Request&) {
            std::lock_guard lock(m);
            Json list = Json::array();
            for (const auto& [id, c] : channels)
                if (c.value("type", 0) == 11) list.push_back(c);
            return http::Response::json(Json{{"threads", list}, {"members", Json::array()}});
        });
        r.get("/channels/:id", [this](http::Request& q) {
            std::lock_guard lock(m);
            auto it = channels.find(q.param("id"));
            if (it == channels.end()) return http::Response::json(Json{{"message", "Unknown Channel"}, {"code", 10003}}, 404);
            return http::Response::json(it->second);
        });
        r.get("/channels/:id/messages/:mid", [this](http::Request& q) {
            std::lock_guard lock(m);
            auto it = starters.find(q.param("mid"));
            if (it == starters.end()) return http::Response::json(Json{{"message", "Unknown Message"}, {"code", 10008}}, 404);
            return http::Response::json(it->second);
        });
        r.get("/channels/:id/messages", [this](http::Request& q) {
            std::lock_guard lock(m);
            Json list = Json::array();
            for (const auto& [id, s] : starters)
                if (s.value("channel_id", "") == q.param("id")) list.push_back(s);
            return http::Response::json(list);
        });
        r.post("/channels/:id/messages", [this](http::Request& q) {
            std::lock_guard lock(m);
            Json b = Json::parse(q.body);
            b["channel"] = q.param("id");
            posts.push_back(b);
            auth_headers.push_back(q.headers.get("authorization").value_or(""));
            const std::string id = "m" + std::to_string(++message_seq);
            return http::Response::json(Json{{"id", id}, {"channel_id", q.param("id")}, {"content", b.value("content", "")}, {"timestamp", "t"},
                                             {"author", Json{{"id", "bot"}, {"username", "di-bot"}, {"bot", true}}}});
        });
        r.put("/channels/:c/messages/:m/reactions/:e/@me", [this](http::Request& q) {
            std::lock_guard lock(m);
            reactions.push_back(q.param("c") + "/" + q.param("m") + "/" + q.param("e"));
            return http::Response::empty(204);
        });
        r.get("/channels/:id/webhooks", [](http::Request&) { return http::Response::json(Json::array()); });
        r.post("/channels/:id/webhooks", [](http::Request& q) {
            return http::Response::json(Json{{"id", "wh-" + q.param("id")}, {"type", 1}, {"name", "Discutere議論"}, {"token", "fake-webhook-token"},
                                             {"channel_id", q.param("id")}});
        });
        r.post("/webhooks/:id/:token", [this](http::Request& q) {
            std::lock_guard lock(m);
            Json b = Json::parse(q.body);
            b["thread_id"] = q.query("thread_id").value_or("");
            webhooks.push_back(b);
            const std::string id = "w" + std::to_string(++message_seq);
            return http::Response::json(Json{{"id", id}, {"channel_id", b["thread_id"]}, {"content", b.value("content", "")}, {"timestamp", "t"},
                                             {"author", Json{{"id", "wh"}, {"username", b.value("username", "")}, {"bot", true}}}});
        });
        r.patch("/webhooks/:app/:token/messages/@original", [this](http::Request& q) {
            std::lock_guard lock(m);
            edits.push_back(Json::parse(q.body));
            return http::Response::json(Json{{"id", "orig"}, {"channel_id", "x"}, {"content", ""}, {"timestamp", "t"},
                                             {"author", Json{{"id", "bot"}, {"username", "di-bot"}}}});
        });
        r.post("/interactions/:id/:token/callback", [this](http::Request& q) {
            std::lock_guard lock(m);
            callbacks.push_back(Json{{"id", q.param("id")}, {"body", Json::parse(q.body)}});
            return http::Response::empty(204);
        });
        r.patch("/channels/:id", [this](http::Request& q) {
            std::lock_guard lock(m);
            Json b = Json::parse(q.body);
            patches.push_back(Json{{"channel", q.param("id")}, {"body", b}});
            auto it = channels.find(q.param("id"));
            Json c = it == channels.end() ? channel_json(q.param("id"), 11, "G1", std::string("F1")) : it->second;
            return http::Response::json(c);
        });
        r.put("/applications/:app/guilds/:g/commands", [this](http::Request& q) {
            std::lock_guard lock(m);
            ++commands_registered;
            return http::Response::json(Json::parse(q.body));
        });
        r.put("/applications/:app/commands", [this](http::Request& q) {
            std::lock_guard lock(m);
            ++commands_registered;
            return http::Response::json(Json::parse(q.body));
        });
        r.get("/gateway/bot", [this](http::Request&) {
            std::lock_guard lock(m);
            Json limit = {{"total", 1000}, {"remaining", 1000}, {"reset_after", 0}, {"max_concurrency", 1}};
            return http::Response::json(Json{{"url", gateway_url}, {"shards", 1}, {"session_start_limit", limit}});
        });
        fake.start();
    }

    di::discord_hook::DiscordApiOptions options(const std::string& token = "fake-bot-token-not-real") const {
        di::discord_hook::DiscordApiOptions o;
        o.token = token;
        o.base_url = fake.base;
        o.allow_private_network = true;
        o.max_retries = 0;
        o.timeout = std::chrono::milliseconds(5'000);
        return o;
    }

    /** 管理フォーラム F1 のスレッド (parent=F1) か、管理外のスレッド (parent=F9)。 */
    void add_thread(const std::string& id, const std::string& parent = "F1", const std::string& name = "周回の議論") {
        std::lock_guard lock(m);
        channels[id] = channel_json(id, 11, "G1", parent, name);
    }
    void add_starter(const std::string& thread, const std::string& content, bool bot = false) {
        std::lock_guard lock(m);
        starters[thread] = Json{{"id", thread}, {"channel_id", thread}, {"guild_id", "G1"}, {"content", content}, {"timestamp", "2026-01-01T00:00:00.000Z"},
                                {"author", Json{{"id", "u1"}, {"username", "alice"}, {"bot", bot}}}, {"mentions", Json::array()},
                                {"attachments", Json::array()}};
    }

    std::vector<std::string> post_contents(const std::string& channel = "") {
        std::lock_guard lock(m);
        std::vector<std::string> out;
        for (const auto& p : posts)
            if (channel.empty() || p.value("channel", "") == channel) out.push_back(p.value("content", ""));
        return out;
    }
    std::size_t post_count() {
        std::lock_guard lock(m);
        return posts.size();
    }
    template <class T>
    std::vector<T> snapshot(const std::vector<T>& v) {
        std::lock_guard lock(m);
        return v;
    }
};

/** 条件が満たされるまで待つ (期限内に満たされれば true)。 */
template <class F>
bool wait_until(F&& cond, std::chrono::milliseconds timeout = std::chrono::milliseconds(10'000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

} // namespace w5u
