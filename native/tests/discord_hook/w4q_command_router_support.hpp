#pragma once
// w4q_command_router_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// w4-Q: discord-hook/command-router.ts の移植の検証。
// tests/discord-hook/command-router.test.ts の移植 + admin allowlist の境界 (未設定 / 空 / 一致 / 不一致)・
// command_defs との一致 (登録だけ・handler だけを防ぐ)・interaction JSON の入口・平文/フォーラムの取り込み・
// golden (Node の routeSlashCommand の返信文と一致)。
#include "check.hpp"
#include "w4q_support.hpp"

#include <algorithm>
#include <atomic>
#include <set>

#include "base/strings.hpp"
#include "discord_hook/command_defs.hpp"
#include "discord_hook/command_router.hpp"
#include "persona_engine/index.hpp"
#include "persona_engine/llm/mock.hpp"

using namespace di;
using namespace di::discord_hook;

namespace w4q_command_router_support {

class NoopProvider : public persona_engine::DiscussionContextProvider {
public:
    std::vector<persona_engine::ContextHypothesis> list_active_hypotheses(const std::string&, int) override { return {}; }
    std::vector<persona_engine::ContextGap> list_recent_gaps(const std::string&, int) override { return {}; }
    std::vector<persona_engine::ContextUtterance> list_recent_utterances(const std::string&, const std::string&, int) override { return {}; }
    std::string propose_hypothesis(const persona_engine::ProposeHypothesisInput&) override { return ""; }
    std::string post_utterance(const persona_engine::PostUtteranceInput&) override { return ""; }
};

std::shared_ptr<persona_engine::PersonaEngineHandle> make_engine(const std::string& pe_path) {
    persona_engine::PersonaEngineOptions o;
    o.db = sql::Database::open(pe_path);
    o.llm = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{});
    o.contextProvider = std::make_shared<NoopProvider>();
    o.workspaceId = "ws";
    o.enableLlm = false;
    return persona_engine::create_persona_engine(o);
}

CommandRouterDeps deps_with(const w4q::Env& env, std::vector<std::string> admin_ids,
                            std::shared_ptr<persona_engine::PersonaEngineHandle> engine = nullptr) {
    CommandRouterDeps d;
    d.workspaceId = "ws";
    d.adminIds = std::move(admin_ids);
    d.getEngine = [engine] { return engine; };
    const auto kg = env.kg;
    const auto events = env.dir.file("events.jsonl");
    d.openCore = [kg, events] { return core::create_core(kg, events); };
    // fire-and-forget はテストでは同じスレッドで走らせる
    d.submitBackground = [](std::function<void()> fn) {
        fn();
        return true;
    };
    return d;
}

InboundSlashCommand slash(const std::string& name, const std::optional<std::string>& user = std::string("admin1"), std::string args = "") {
    InboundSlashCommand c;
    c.name = name;
    c.argsText = std::move(args);
    c.userId = user;
    c.guildId = "g1";
    c.channelId = "ch1";
    return c;
}

DiscordInboundMessage message(const std::string& channel, const std::string& content, const std::string& id = "m1") {
    DiscordInboundMessage m;
    m.id = id;
    m.channelId = channel;
    m.author = {"u1", "alice", std::nullopt};
    m.content = content;
    m.timestamp = "1970-01-01T00:00:00.000Z";
    return m;
}

std::int64_t count(w4q::Env& env, const std::string& sql) {
    auto c = env.core();
    const auto n = c->raw().get(sql)->i64("n");
    c->close();
    return n;
}

} // namespace w4q_command_router_support










// ─── interaction JSON の入口 ─────────────────────────────────────────────

namespace w4q_command_router_support {
class RecordingReplier : public SlashReplier {
public:
    std::vector<SlashReply> replies;
    void reply(const SlashReply& r) override { replies.push_back(r); }
};

Json interaction(const std::string& name, const Json& options, const Json& user = Json{{"id", "admin1"}, {"username", "a"}}) {
    return Json{{"id", "i1"},
                {"token", "fake-interaction-token"},
                {"application_id", "app"},
                {"type", 2},
                {"guild_id", "g1"},
                {"channel_id", "ch1"},
                {"member", Json{{"user", user}}},
                {"data", Json{{"name", name}, {"options", options}}}};
}
} // namespace w4q_command_router_support
