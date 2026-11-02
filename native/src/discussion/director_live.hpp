#pragma once
// DiscussionDirector の live アダプタ (src/discussion/director-live.ts。Discord + claude -p)。
//
// - LLM       : ClaudeCliClient (claude -p、メンバーの model を --model で渡す)
// - 発話投稿  : post_discord_webhook (persona の人間名で投稿、フォーラムスレッド対応)
// - 続行/停止 : Discord ボタン [続ける][止める] の interaction (無応答はタイムアウト→停止)
//
// discord.js の代わりに DiscordApi (REST v10) を使う。ボタンの interaction は Gateway の担当が受けて
// ButtonInteraction に詰めて handle_button() に渡す (Gateway への依存はこのヘッダに無い)。
// start() は議論が終わるまで呼んだスレッドで進行する (TS の `void runner.start()` は background_executor に渡す)。
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "config.hpp"
#include "discord_hook/discord_api.hpp"
#include "discussion/director.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::discussion {

using DiscussionConfig = decltype(config::DiscutereConfig::discussion);

/** ボタンの interaction (Gateway の INTERACTION_CREATE から必要な値だけ)。 */
struct ButtonInteraction {
    /** interaction id */
    std::string id;
    /** interaction token (秘密。ログに出さない) */
    std::string token;
    std::string customId;
    std::optional<std::string> channelId;
};

struct DebateRunnerOptions {
    std::shared_ptr<discord_hook::DiscordApi> api;
    DiscussionConfig discussion;
    std::optional<std::string> gitBashPath;
    std::optional<long long> claudeCliTimeoutMs;
    /** テスト用の差し替え (省略時は ClaudeCliClient)。 */
    persona_engine::LLMClientPtr llm;
    /** テスト用の差し替え (省略時は crypto::random_unit)。 */
    Rng rng;
};

/** 続行/停止ボタンの custom_id の接頭辞。 */
inline constexpr const char* DEBATE_CUSTOM_PREFIX = "debate";

/** 続行の問いのメッセージ本文 (ボタン付き)。テスト用に公開。 */
Json build_continue_message(double expected_utterances, const ContinueContext& ctx, const std::string& token);

class DebateRunner {
public:
    explicit DebateRunner(DebateRunnerOptions options);
    ~DebateRunner();
    DebateRunner(const DebateRunner&) = delete;
    DebateRunner& operator=(const DebateRunner&) = delete;

    /** チャンネル (or フォーラムスレッド) で議論を開始し、終わるまで進める。失敗は warn ログ (例外は投げない)。 */
    void start(const std::string& channel_id, const std::string& topic);
    /** button interaction を処理する。続行/停止ボタンなら true。 */
    bool handle_button(const ButtonInteraction& interaction);
    /** 進行中の議論数。 */
    int active_count() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

std::unique_ptr<DebateRunner> create_debate_runner(DebateRunnerOptions options);

} // namespace di::discussion
