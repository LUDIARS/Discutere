#pragma once
// @spec Di管理フォーラムの受信境界
// Discord Gateway (WebSocket) transport — src/discord-hook/gateway.ts の移植。
//
// bot token で Gateway に常時接続して push でイベントを受ける (公開 URL・署名検証は不要)。
// 接続・heartbeat・resume・重複排除・送信制限は gateway_client (w4-T) が持ち、ここは TS と同じイベント処理を載せる:
//   - READY (= discord.js の ClientReady、1 度だけ): slash 自動登録 / monitor カード / 自動作成チャンネル /
//     議論フォーラムとタグの ensure → guild ごとの Di 管理フォーラム ID の解決 / ゲーム感想カテゴリ。
//   - THREAD_CREATE → フォーラム starter の解析 → フロー起動 (flow/discord_live) か議論タイプ選択メニュー。
//   - MESSAGE_CREATE → ペーパーレビューの返信・やり直し・壁打ち・調整指示 (bot メンション)・crawl・取り込み。
//   - INTERACTION_CREATE → 選択メニュー (flow-pick / flow-settings)・ボタン・モーダル・slash (command_router)。
//   - MESSAGE_REACTION_ADD → ✅ 承認 / 議論意見のスコアリング。
// MESSAGE_CREATE / MESSAGE_REACTION_ADD / INTERACTION_CREATE / THREAD_CREATE は必ず forum_event_gate
// (Di 管理フォーラムの guild ID + 親 forum ID の照合、空は全拒否、失敗は拒否) を通してからハンドラへ渡す。
//
// スレッド: gateway_client の制御スレッドでは振り分けだけを行い (dispatch_in_background=false)、ハンドラは
// 2 本の上限つきキューで実行する:
//   - 起動系キュー (THREAD_CREATE / INTERACTION_CREATE / ✅ のリアクション): 議論の起動・承認・3 秒期限の応答。
//   - 通常キュー (MESSAGE_CREATE / その他のリアクション)。
// 通常のイベントが溢れても議論の起動は落ちない。LLM を待つ重い仕事 (フローの起動・ペーパー調整・診断) は
// flowLive.submitBackground (既定 background_executor) に渡す。
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json.hpp"
#include "config.hpp"
#include "discord_hook/command_router.hpp"
#include "discord_hook/crawl_channel.hpp"
#include "discord_hook/discord_api.hpp"
#include "discord_hook/forum_monitor.hpp"
#include "discord_hook/gateway_client.hpp"
#include "discord_hook/managed_forum_scope.hpp"
#include "flow/discord_live.hpp"
#include "mechanics_check/tool.hpp"
#include "level_check/tool.hpp"

namespace di::discord_hook {

/** フォーラム starter の適用タグの解析結果 (flow/entry-discord の ParsedForumEntry と同じ形)。 */
struct ParsedForumEntry {
    /** 解決した議論タイプ。判定できなければ nullopt (= 選択メニューを出す)。 */
    std::optional<flow::FlowKind> flow;
    std::vector<flow::FlowTag> tags;
};
struct ParsedRoundsTurns {
    std::optional<double> rounds;
    std::optional<double> turnsPerRound;
};

/**
 * flow/entry-discord.ts の 3 関数 (並行担当が移す)。main が結線する。未設定なら
 * parseForumEntry は「タイプ未判定・タグなし」(= 選択メニュー)、他は未指定として扱う。
 */
struct ForumEntryParsers {
    std::function<ParsedForumEntry(const std::vector<std::string>& appliedTagNames)> parseForumEntry;
    std::function<ParsedRoundsTurns(const std::string& text)> parseRoundsTurns;
    std::function<std::optional<std::vector<std::string>>(const std::string& text)> parseOpponents;
};

struct ReactionInfo {
    std::string messageId;
    std::string channelId;
    std::string emoji;
    std::string userId;
};

struct GameFeedbackInput {
    std::string gameTitle;
    std::string content;
    double createdAt = 0;
};

using ForumConfig = decltype(config::DiscutereConfig::discord.forum);
using GameFeedbackConfig = decltype(config::DiscutereConfig::discord.gameFeedback);
using DebateConfig = decltype(config::DiscutereConfig::discussion);

struct DiscordGatewayDeps {
    /** TS の `extends CommandRouterDeps` (workspaceId / adminIds / discussionChannelIds / ...)。 */
    CommandRouterDeps router;
    /** Gateway 接続用 bot token。空なら起動しない。ログに出さない。 */
    std::string botToken;
    /** slash command 登録用 application id (未設定なら READY の application.id / user.id)。 */
    std::optional<std::string> applicationId;
    /** 運用 guild id 群 (複数サーバ対応)。"dm" と空は除く。 */
    std::vector<std::string> guildIds;
    /** メッセージへのリアクション付与イベント (議論意見スコアリング用)。 */
    std::function<void(const ReactionInfo&)> onReaction;
    /** データクロール用チャンネル id。 */
    std::vector<std::string> crawlChannelIds;
    /** クロール取り込みの依存 (未設定ならクロール skip)。 */
    std::optional<CrawlDeps> crawlDeps;
    /** フォーラム集約設定。enabled なら解決済みの Di 管理フォーラムだけを監視する。 */
    std::optional<ForumConfig> forum;
    /** 新フロー (議論/改善/学習/壁打ち) の Discord live 実行に必要な依存。未設定なら forum 起動は skip。 */
    std::optional<flow::FlowDiscordDeps> flowLive;
    /** entry-discord の解析 (main が結線)。 */
    ForumEntryParsers entryParsers;
    std::shared_ptr<const mechanics_check::MechanicsCheckDependencies> mechanicsCheck;
    std::shared_ptr<const level_check::LevelCheckDependencies> levelCheck;
    /** /debate のパーティ議論設定 (未設定なら /debate 無効)。 */
    std::optional<DebateConfig> debate;
    std::optional<std::string> gitBashPath;
    std::optional<GameFeedbackConfig> gameFeedback;
    /** ゲーム感想の収集コールバック (投稿者は渡さない = 匿名)。 */
    std::function<void(const GameFeedbackInput&)> onGameFeedback;

    // ── C++ 版の注入点 ──
    /** REST の設定 (token は botToken で上書き)。テストで base_url を偽サーバへ向ける。 */
    DiscordApiOptions rest;
    /** Gateway の接続設定 (token・on_dispatch・dispatch_in_background は上書き)。テストで偽 Gateway に向ける。 */
    GatewayClientOptions gatewayOptions;
    /** false なら Gateway に接続しない (テストで handle_dispatch に直接イベントを渡す)。 */
    bool connect = true;
    /** guild ごとの monitor 状態カードを置くか (既定 true。テストは false)。 */
    bool monitorCards = true;
    /** 起動系キュー・通常キューのスレッド数と上限。 */
    std::size_t priorityThreads = 2;
    std::size_t priorityQueue = 1024;
    std::size_t normalThreads = 4;
    std::size_t normalQueue = 512;
    /** チャンネルの同一性 (id / guild / parent / type) のキャッシュ期間。0 でキャッシュしない。 */
    std::chrono::milliseconds channelCacheTtl{5 * 60'000};
    std::size_t channelCacheCapacity = 2'000;
    /** 待ち状態 (議論タイプ選択・進行量設定) の上限件数 (超えたら古いものから捨てる)。 */
    std::size_t maxPendingStarts = 1'000;
    /** starter 取得のリトライ待ち (既定 500ms の sleep)。 */
    std::function<void(std::chrono::milliseconds)> sleep;
};

struct GatewayEventStats {
    std::uint64_t priorityQueued = 0;
    std::uint64_t normalQueued = 0;
    /** 起動系キューが満杯で捨てた数 (error ログも出す)。 */
    std::uint64_t priorityDropped = 0;
    std::uint64_t normalDropped = 0;
    /** 管理フォーラム境界で捨てた数。 */
    std::uint64_t gateRejected = 0;
};

struct SweepUnseededResult {
    int scanned = 0;
    int seeded = 0;
};

class DiscordGateway {
public:
    struct Impl;
    explicit DiscordGateway(std::shared_ptr<Impl> impl);
    ~DiscordGateway();
    DiscordGateway(const DiscordGateway&) = delete;
    DiscordGateway& operator=(const DiscordGateway&) = delete;

    /** 接続を閉じ、キューを止め、monitor を止める。何度呼んでもよい。 */
    void stop();
    /**
     * 収束したフォーラムポストを締める (archive+lock + まとめ転記)。
     * scene が管理フォーラムのスレッドでなければ {closed:false, reason:"outside managed forum"}。
     */
    FinalizeForumPostResult finalize_forum_post(const std::optional<std::string>& scene, const std::string& summary,
                                                const std::string& title);
    /** 管理フォーラム内のメッセージに絵文字リアクションを付ける (best-effort、失敗は握り潰す)。 */
    void react_to_message(const std::string& channel_id, const std::string& message_id, const std::string& emoji);
    /** 未検知 (👀 が付いていない) 投稿を種まきする再スイープ (FEATURE ⑤b)。 */
    SweepUnseededResult sweep_unseeded(std::optional<int> limit = std::nullopt);

    /** Gateway の dispatch を 1 件処理する (制御スレッドから呼ばれる。テストは直接渡せる)。 */
    void handle_dispatch(const std::string& event, const Json& d);
    /** READY 処理が終わったか (テスト用)。 */
    bool ready_done() const;
    /** 起動系・通常キューが空になるまで待つ (テスト用)。期限内に空になれば true。 */
    bool wait_idle(std::chrono::milliseconds timeout) const;
    ManagedForumIds managed_forum_ids() const;
    std::vector<std::string> crawl_channel_ids() const;
    GatewayEventStats event_stats() const;
    /** 接続中の Gateway の状態 (connect=false なら既定値)。 */
    GatewayStats gateway_stats() const;
    /** 共有の REST クライアント。 */
    std::shared_ptr<DiscordApi> api() const;

private:
    std::shared_ptr<Impl> impl_;
};

/** Gateway を起動する。botToken が空なら nullptr を返して skip (= HTTP health 等は従来通り)。 */
std::unique_ptr<DiscordGateway> start_discord_gateway(DiscordGatewayDeps deps);

/** 仕様書解析 (③) に回せるテキスト系添付か (画像/動画/バイナリは除外)。 */
bool is_text_like_attachment(const std::string& name, const std::optional<std::string>& content_type);

/** modal submit の d.data から text input の値を集める (custom_id → value。入れ子の components を辿る)。 */
std::map<std::string, std::string> modal_field_values(const Json& data);

/** 進行量モーダルの値 (Number(s.trim()) が有限かつ正なら採る)。 */
std::optional<double> positive_number_field(const std::string& raw);

} // namespace di::discord_hook
