#pragma once
// discutere-monitor 状態カード (src/discord-hook/monitor-card.ts の移植)。
//
// Discord 接続時、「状態」カテゴリに discutere-monitor チャンネルを冪等生成し、進行中セッション数
// (sessions.ended_at IS NULL) と未処理仮説数 (hypotheses.status = 'proposed') と最終更新時間を 1 枚の embed
// カードとして定期更新する。Discord 操作はすべて DiscordApi (REST)。discord.js のキャッシュは使わず都度取りに行く。
// 定期実行は global_scheduler() で起こし、仕事 (DB + REST) は background_executor() で走らせる。
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "base/json.hpp"
#include "discord_hook/discord_api.hpp"
#include "persona_engine/facilitator/periodic.hpp"

namespace di::discord_hook {

inline constexpr const char* STATUS_CATEGORY_NAME = "状態";
inline constexpr const char* MONITOR_CHANNEL_NAME = "discutere-monitor";
inline constexpr long long MONITOR_UPDATE_INTERVAL_MS = 60'000;
inline constexpr int MONITOR_FETCH_LIMIT = 100;

struct MonitorCardDeps {
    std::shared_ptr<DiscordApi> api;
    /** 集計対象 workspace (command-router と同じ匿名 workspace) */
    std::string workspaceId;
    /** discutere-monitor 状態カードの設置先ギルド */
    std::string guildId;
};

/** EmbedBuilder(...).toJSON() と同じ形 ({title, color, fields, footer, timestamp})。 */
Json build_monitor_embed(std::int64_t active_sessions, std::int64_t pending_hypotheses, std::int64_t now_ms);

class MonitorCard {
public:
    explicit MonitorCard(MonitorCardDeps deps);
    ~MonitorCard();
    MonitorCard(const MonitorCard&) = delete;
    MonitorCard& operator=(const MonitorCard&) = delete;

    /** 即時に 1 回 + 以後 interval ごとに更新する (既に動いていれば何もしない)。 */
    void start(long long interval_ms = MONITOR_UPDATE_INTERVAL_MS);
    void stop();
    /** 1 回分の更新 (失敗は warn して握る)。テスト用に公開。 */
    void tick();

private:
    struct Counts {
        std::int64_t activeSessions = 0;
        std::int64_t pendingHypotheses = 0;
    };
    Counts counts();
    std::optional<Channel> ensure_monitor_channel();
    std::optional<std::string> resolve_existing_monitor_message(const std::string& channel_id);

    MonitorCardDeps deps_;
    std::mutex mutex_;  // tick の直列化 + message_id_
    std::optional<std::string> message_id_;
    persona_engine::facilitator::PeriodicTask timer_;
};

} // namespace di::discord_hook
