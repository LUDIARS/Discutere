#pragma once
// w3m_discord_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// w3-M: discord-hook の command-defs / register-commands / level-check / mechanics-check / discussion-bridge /
// directive-handler / monitor-card と level-check/tool・mechanics-check/tool の移植の検証。
// tests/discord-hook/{level-check,mechanics-check}.test.ts・tests/{level,mechanics}-check/tool.test.ts の観点 +
// slash 定義の golden (1 バイト一致)・登録 (guild/global)・投稿ブリッジ・調整指示・状態カード (偽 Discord)。
#include "check.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/test_util.hpp"
#include "db/connection.hpp"
#include "db/repository.hpp"
#include "discord_hook/command_defs.hpp"
#include "discord_hook/directive_handler.hpp"
#include "discord_hook/discussion_bridge.hpp"
#include "discord_hook/facilitator_directives.hpp"
#include "discord_hook/level_check.hpp"
#include "discord_hook/mechanics_check.hpp"
#include "discord_hook/monitor_card.hpp"
#include "discord_hook/poster.hpp"
#include "discord_hook/register_commands.hpp"
#include "discord_hook/slash_interaction.hpp"
#include "level_check/tool.hpp"
#include "mechanics_check/tool.hpp"
#include "persona_engine/llm/mock.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::discord_hook;
using Variant = std::variant<std::string, persona_engine::LLMResult>;

namespace w3m_discord_support {

const char* kFakeToken = "fake-bot-token-not-real";

/** キー順を無視して比べる。 */
bool same(const Json& a, const Json& b) { return nlohmann::json::parse(a.dump()) == nlohmann::json::parse(b.dump()); }

Json fixture_case(const std::string& file, const std::string& name) {
    for (const auto& c : test::load_fixture(file))
        if (c["name"] == name) return c;
    throw test::Failure("fixture case missing: " + name);
}

persona_engine::LLMClientPtr llm_text(const std::string& text, std::atomic<int>* calls = nullptr) {
    return std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [text, calls](const persona_engine::LLMInvokeArgs&) -> Variant {
            if (calls) ++*calls;
            return text;
        }});
}

struct RecordingResponder : InteractionResponder {
    std::vector<std::string>* lifecycle;
    std::vector<DiagnosticReply> replies;
    bool fail_defer = false;
    bool fail_edit = false;
    explicit RecordingResponder(std::vector<std::string>* l) : lifecycle(l) {}
    void defer_ephemeral() override {
        lifecycle->push_back("defer");
        if (fail_defer) throw std::runtime_error("transport failed");
    }
    void edit_reply(const DiagnosticReply& r) override {
        lifecycle->push_back("edit");
        if (fail_edit) throw std::runtime_error("transport failed");
        replies.push_back(r);
    }
};

SlashInteraction slash(const std::string& name, std::vector<std::pair<std::string, std::optional<std::string>>> opts) {
    SlashInteraction s;
    s.id = "i1";
    s.token = "fake-interaction-token";
    s.applicationId = "app";
    s.commandName = name;
    for (auto& [k, v] : opts)
        if (v) s.options.push_back({k, option_type::String, Json(*v)});
    return s;
}

/** 偽 Discord (登録・interaction 応答・投稿・状態カード)。 */
struct FakeDiscord {
    w1d::FakeServer fake;
    std::mutex m;
    std::vector<std::pair<std::string, std::string>> puts;  // path, body
    std::vector<std::string> callbacks;
    std::vector<std::pair<std::string, std::string>> edits;  // content-type, body
    Json channels = Json::array();
    Json channel_messages = Json::array();
    std::vector<std::string> deleted;
    std::vector<Json> created_messages;
    std::vector<Json> edited_messages;
    std::vector<Json> webhook_posts;
    bool guild_missing = false;
    bool webhook_forbidden = false;
    int next = 500;

    FakeDiscord() {
        auto& r = *fake.router;
        auto put = [this](http::Request& q) {
            std::lock_guard l(m);
            CHECK_EQ(q.header("Authorization").value_or(""), std::string("Bot ") + kFakeToken);
            puts.push_back({q.path, q.body});
            return http::Response::bytes(q.body, "application/json");
        };
        r.put("/applications/:app/commands", put);
        r.put("/applications/:app/guilds/:g/commands", put);
        r.post("/interactions/:id/:token/callback", [this](http::Request& q) {
            std::lock_guard l(m);
            CHECK(!q.header("Authorization"));
            callbacks.push_back(q.body);
            return http::Response::empty(204);
        });
        r.patch("/webhooks/:app/:token/messages/@original", [this](http::Request& q) {
            std::lock_guard l(m);
            edits.push_back({q.header("content-type").value_or(""), q.body});
            return http::Response::json(Json{{"id", "orig"}, {"channel_id", "c"}});
        });
        r.get("/users/@me", [](http::Request&) { return http::Response::json(Json{{"id", "bot"}, {"username", "Di"}, {"bot", true}}); });
        r.get("/guilds/:g", [this](http::Request& q) {
            if (guild_missing) return http::Response::json(Json{{"message", "Unknown Guild"}, {"code", 10004}}, 404);
            return http::Response::json(Json{{"id", q.param("g")}, {"name", "G"}, {"roles", Json::array()}});
        });
        r.get("/guilds/:g/channels", [this](http::Request&) {
            std::lock_guard l(m);
            return http::Response::json(channels);
        });
        r.post("/guilds/:g/channels", [this](http::Request& q) {
            std::lock_guard l(m);
            Json ch = Json::parse(q.body);
            ch["id"] = std::to_string(next++);
            ch["guild_id"] = q.param("g");
            channels.push_back(ch);
            return http::Response::json(ch, 201);
        });
        r.get("/channels/:c/messages", [this](http::Request& q) {
            std::lock_guard l(m);
            CHECK_EQ(q.query("limit").value_or(""), std::string("100"));
            return http::Response::json(channel_messages);
        });
        r.del("/channels/:c/messages/:m", [this](http::Request& q) {
            std::lock_guard l(m);
            deleted.push_back(q.param("m"));
            return http::Response::empty(204);
        });
        r.get("/channels/:c/messages/:m", [this](http::Request& q) {
            std::lock_guard l(m);
            for (const auto& msg : channel_messages)
                if (msg["id"] == q.param("m")) return http::Response::json(msg);
            for (const auto& msg : created_messages)
                if (msg["id"] == q.param("m")) return http::Response::json(msg);
            return http::Response::json(Json{{"message", "Unknown Message"}, {"code", 10008}}, 404);
        });
        r.patch("/channels/:c/messages/:m", [this](http::Request& q) {
            std::lock_guard l(m);
            edited_messages.push_back(Json::parse(q.body));
            return http::Response::json(Json{{"id", q.param("m")}, {"channel_id", q.param("c")}});
        });
        r.post("/channels/:c/messages", [this](http::Request& q) {
            std::lock_guard l(m);
            Json body = Json::parse(q.body);
            Json msg{{"id", std::to_string(next++)}, {"channel_id", q.param("c")}, {"author", {{"id", "bot"}, {"username", "Di"}}}};
            msg["content"] = body.value("content", "");
            created_messages.push_back(msg);
            Json rec = body;
            rec["_channel"] = q.param("c");
            created_messages.back()["_body"] = rec;
            return http::Response::json(msg);
        });
        r.get("/channels/:c", [](http::Request& q) {
            if (q.param("c") == "thread") return http::Response::json(Json{{"id", "thread"}, {"type", 11}, {"parent_id", "forum"}});
            return http::Response::json(Json{{"id", q.param("c")}, {"type", 0}});
        });
        r.get("/channels/:c/webhooks", [this](http::Request&) {
            if (webhook_forbidden) return http::Response::json(Json{{"message", "Missing Permissions"}, {"code", 50013}}, 403);
            return http::Response::json(Json::array({Json{{"id", "w1"}, {"name", "Discutere議論"}, {"token", "fake-webhook-token"}}}));
        });
        r.post("/webhooks/:id/:token", [this](http::Request& q) {
            std::lock_guard l(m);
            Json rec = Json::parse(q.body);
            rec["_query"] = q.query_string;
            webhook_posts.push_back(rec);
            return http::Response::json(Json{{"id", "wm1"}, {"channel_id", "x"}});
        });
        fake.start();
    }

    DiscordApiOptions options() const {
        DiscordApiOptions o;
        o.token = kFakeToken;
        o.base_url = fake.base;
        o.allow_private_network = true;
        return o;
    }
};

/** active KG / main DB / config を一時ディレクトリへ向ける。 */
struct Env {
    test::TempDir dir;
    std::string kg;
    explicit Env(const std::optional<std::string>& bot_token = std::nullopt) {
        kg = dir.file("kg.kuzu");
        env::set_for_test("DISCUTERE_CONFIG", std::string_view(dir.file("no-config.json")));
        env::set_for_test("DISCATIER_KUZU_PATH", std::string_view(kg));
        env::set_for_test("DATABASE_PATH", std::string_view(dir.file("main.db")));
        env::set_for_test("DISCUTERE_DISCORD_BOT_TOKEN",
                          bot_token ? std::optional<std::string_view>(*bot_token) : std::optional<std::string_view>());
        config::reset_config_for_test();
        db::reset_main_db_for_test();
    }
    ~Env() {
        set_discussion_bridge_api_factory(nullptr);
        clear_webhook_cache();
        db::reset_main_db_for_test();
        env::set_for_test("DISCUTERE_DISCORD_BOT_TOKEN", std::nullopt);
        config::reset_config_for_test();
    }
    std::unique_ptr<core::Core> core() { return core::create_core(kg, dir.file("events.jsonl")); }
};

std::string make_session(core::Core& c, const std::string& scene, const std::string& title = "s") {
    core::repositories::SessionInput s;
    s.workspaceId = "knowledge";
    s.title = title;
    s.startedAt = 1;
    s.scene = scene;
    return c.repos.session.create(s);
}

} // namespace w3m_discord_support
