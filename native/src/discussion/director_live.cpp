#include "discussion/director_live.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "discord_hook/poster.hpp"
#include "persona_engine/llm/claude_cli.hpp"

namespace di::discussion {

namespace {

/** DirectorLLM → LLMClient (メンバーの model をそのまま渡す)。 */
class DirectorLlmAdapter : public DirectorLLM {
public:
    explicit DirectorLlmAdapter(persona_engine::LLMClientPtr llm) : llm_(std::move(llm)) {}
    DirectorLLMResult invoke(const DirectorLLMArgs& a) override {
        persona_engine::LLMInvokeArgs args;
        args.system = a.system;
        args.prompt = a.prompt;
        args.model = a.model;
        const auto r = llm_->invoke(args);
        return {r.ok, r.text, r.error};
    }

private:
    persona_engine::LLMClientPtr llm_;
};

/** discord.js の channel.isTextBased() && "send" in channel に当たる種類。 */
bool is_sendable_text_channel(int type) {
    namespace ct = discord_hook::channel_type;
    switch (type) {
    case ct::GuildText:
    case ct::DM:
    case ct::GuildVoice:
    case ct::GuildAnnouncement:
    case ct::AnnouncementThread:
    case ct::PublicThread:
    case ct::PrivateThread:
    case ct::GuildStageVoice:
        return true;
    default:
        return false;
    }
}

Json button(const std::string& custom_id, const std::string& label, int style) {
    Json b = Json::object();
    b["type"] = 2;
    b["custom_id"] = custom_id;
    b["label"] = label;
    b["style"] = style;
    return b;
}

} // namespace

Json build_continue_message(double expected_utterances, const ContinueContext& ctx, const std::string& token) {
    Json row = Json::object();
    row["type"] = 1;
    // ButtonStyle.Success = 3 / Danger = 4
    row["components"] = Json::array({button(std::string(DEBATE_CUSTOM_PREFIX) + ":cont:" + token, "続ける", 3),
                                     button(std::string(DEBATE_CUSTOM_PREFIX) + ":stop:" + token, "止める", 4)});
    Json msg = Json::object();
    msg["content"] = "🗳️ 想定発話数 (" + js_number_to_string(expected_utterances) + ") に到達しました " + "(ラウンド" +
                     std::to_string(ctx.round) + " / 累計" + std::to_string(ctx.total) + "発話)。議論を続けますか?\n" +
                     "続ける場合、司会を除くメンバーを半数入れ替えます。";
    msg["components"] = Json::array({row});
    return msg;
}

struct DebateRunner::Impl {
    DebateRunnerOptions opts;
    std::shared_ptr<DirectorLlmAdapter> llm;

    struct PendingContinue {
        std::string channelId;
        bool resolved = false;
        bool value = false;
    };
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::string, std::shared_ptr<PendingContinue>> pending;
    std::uint64_t token_seq = 0;
    std::atomic<int> active{0};
    bool closed = false;

    PartyConfig party_config() const {
        const auto& d = opts.discussion;
        PartyConfig c;
        c.expectedUtterances = d.expectedUtterances;
        c.keymanCount = static_cast<int>(d.keymanCount);
        c.facilitatorModel = d.facilitatorModel;
        c.keymanModel = d.keymanModel;
        for (const auto& m : d.opinionModels) c.opinionModels.push_back({m.model, m.weight});
        c.minTotal = static_cast<int>(d.minTotal);
        return c;
    }

    void post_as(const std::string& channel_id, const std::string& username, const std::string& content) {
        // スレッドは webhook を持てないので、親チャンネルの webhook + thread_id で流す。
        const auto target = discord_hook::resolve_webhook_target(*opts.api, channel_id);
        const auto wh = discord_hook::ensure_channel_webhook(*opts.api, target.webhookChannelId);
        discord_hook::DiscordWebhookPostArgs args;
        args.webhookId = wh.id;
        args.webhookToken = wh.token;
        args.username = username;
        args.content = content;
        args.threadId = target.threadId;
        discord_hook::post_discord_webhook(*opts.api, args);
    }

    bool ask_continue(const std::string& channel_id, const ContinueContext& ctx) {
        std::string token;
        {
            std::lock_guard lock(mutex);
            token = std::to_string(time::now_ms()) + "-" + std::to_string(token_seq++);
        }
        const auto channel = opts.api->get_channel(channel_id);
        if (!is_sendable_text_channel(channel.type)) return false;
        auto entry = std::make_shared<PendingContinue>();
        entry->channelId = channel_id;
        {
            std::lock_guard lock(mutex);
            pending[token] = entry;
        }
        try {
            opts.api->create_message(channel_id, build_continue_message(opts.discussion.expectedUtterances, ctx, token));
        } catch (...) {
            std::lock_guard lock(mutex);
            pending.erase(token);
            throw;
        }
        std::unique_lock lock(mutex);
        const auto timeout = std::chrono::milliseconds(static_cast<long long>(std::max(0.0, opts.discussion.continueTimeoutMs)));
        cv.wait_for(lock, timeout, [&] { return entry->resolved || closed; });
        pending.erase(token);
        return entry->resolved && entry->value;  // 無応答は停止
    }

    void respond(const ButtonInteraction& interaction, const Json& body) {
        try {
            opts.api->create_interaction_response(interaction.id, interaction.token, body);
        } catch (...) {
            // TS の .catch(() => {}) と同じく応答の失敗は無視する
        }
    }
};

DebateRunner::DebateRunner(DebateRunnerOptions options) : impl_(std::make_shared<Impl>()) {
    if (!options.api) throw std::invalid_argument("create_debate_runner: api required");
    if (!options.llm) {
        persona_engine::ClaudeCliClientOptions o;
        o.defaultTimeoutMs = options.claudeCliTimeoutMs.value_or(90'000);
        o.gitBashPath = options.gitBashPath;
        options.llm = std::make_shared<persona_engine::ClaudeCliClient>(o);
    }
    if (!options.rng) options.rng = [] { return crypto::random_unit(); };
    impl_->llm = std::make_shared<DirectorLlmAdapter>(options.llm);
    impl_->opts = std::move(options);
}

DebateRunner::~DebateRunner() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->closed = true;
    }
    impl_->cv.notify_all();
}

void DebateRunner::start(const std::string& channel_id, const std::string& topic) {
    auto impl = impl_;
    impl->active.fetch_add(1);
    struct Done {
        std::atomic<int>& n;
        ~Done() { n.fetch_sub(1); }
    } done{impl->active};
    try {
        impl->post_as(channel_id, "司会", "📣 議論を始めます: 「" + topic + "」");
        DirectorDeps deps;
        deps.llm = impl->llm.get();
        deps.postUtterance = [impl, channel_id](const PartyMember& m, const std::string& text) {
            impl->post_as(channel_id, m.personaName, text);
        };
        deps.postSummary = [impl, channel_id](const PartyMember& m, const std::string& text) {
            impl->post_as(channel_id, m.personaName, "🧷 まとめ: " + text);
        };
        deps.askContinue = [impl, channel_id](const ContinueContext& ctx) { return impl->ask_continue(channel_id, ctx); };
        deps.log = [channel_id](const std::string& msg) { log::info("  [debate " + channel_id + "] " + msg); };
        deps.rng = impl->opts.rng;
        deps.turnDelayMs = static_cast<long long>(impl->opts.discussion.turnDelayMs);
        deps.maxRounds = static_cast<int>(impl->opts.discussion.maxRounds);
        const auto result = run_discussion(DiscussionSpec{topic, impl->party_config()}, deps);
        log::info("  [debate " + channel_id + "] 終了 (" + result.stoppedBy + ", " + std::to_string(result.rounds) + "ラウンド, " +
                  std::to_string(result.utterances.size()) + "発話)");
    } catch (const std::exception& e) {
        log::warn("  [debate " + channel_id + "] 失敗: " + std::string(e.what()));
    } catch (...) {
        log::warn("  [debate " + channel_id + "] 失敗: unknown error");
    }
}

// @spec Di管理フォーラムの受信境界
bool DebateRunner::handle_button(const ButtonInteraction& interaction) {
    const std::string& id = interaction.customId;
    if (!str::starts_with(id, std::string(DEBATE_CUSTOM_PREFIX) + ":")) return false;
    const auto parts = str::split(id, ":");
    const std::string kind = parts.size() > 1 ? parts[1] : "";
    std::shared_ptr<Impl::PendingContinue> p;
    {
        std::lock_guard lock(impl_->mutex);
        if (parts.size() > 2) {
            auto it = impl_->pending.find(parts[2]);
            if (it != impl_->pending.end()) p = it->second;
        }
    }
    if (!p) {
        impl_->respond(interaction, Json{{"type", 4}, {"data", Json{{"content", "この問いかけは期限切れです。"}, {"flags", 64}}}});
        return true;
    }
    // A copied continuation token must not control another thread's discussion.
    if (!interaction.channelId || p->channelId != *interaction.channelId) return true;
    const bool cont = kind == "cont";
    {
        std::lock_guard lock(impl_->mutex);
        if (p->resolved) return true;
        p->resolved = true;
        p->value = cont;
        impl_->pending.erase(parts[2]);
    }
    impl_->cv.notify_all();
    impl_->respond(interaction,
                   Json{{"type", 7},
                        {"data", Json{{"content", cont ? "✅ 続行します (メンバーを半数入れ替えます)" : "🛑 ここで議論を締めます"},
                                      {"components", Json::array()}}}});
    return true;
}

int DebateRunner::active_count() const { return impl_->active.load(); }

std::unique_ptr<DebateRunner> create_debate_runner(DebateRunnerOptions options) {
    return std::make_unique<DebateRunner>(std::move(options));
}

} // namespace di::discussion
