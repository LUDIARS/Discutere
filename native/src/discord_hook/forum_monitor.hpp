#pragma once
// フォーラム議論の入口 + 収束クローズ (src/discord-hook/forum-monitor.ts の移植)。
//
// Di の議論を Discord フォーラムチャンネルに集約する。受信許可は Gateway の管理 forum ID 境界
// (forum_event_gate / managed_forum_scope: guild ID と親 forum ID の組、空は全拒否、取得失敗は拒否) が担当し、
// ここはその門を通ったイベントだけを受ける:
//   - フォーラムの新規ポスト (ThreadCreate, 親=GuildForum) の最初の投稿 (starter) で議論をトリガーする
//     (command_router::route_forum_post 経由 → auto-discussion)。議論が立てば starter に 👀。
//   - ポスト内の後続投稿 (MessageCreate, starter 以外) は進行中議論への参加者発言になる。
//   - 議論が収束したら finalize_forum_post でポストを lock + archive し、まとめを「まとめ投稿」へ転記する。
//
// discord.js の代わりに Gateway の JSON (ThreadCreate / MessageCreate の d) と DiscordApi (REST) を受ける。
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "discord_hook/command_router.hpp"
#include "discord_hook/discord_api.hpp"
#include "discord_hook/managed_forum_scope.hpp"

namespace di::discord_hook {

/** タグ名 → 方向性のマッピング設定 (config `discord.forum`)。 */
struct ForumDirectionConfig {
    /** この名前(部分一致)のタグが付いたら「改善提案」方向。 */
    std::vector<std::string> improvementTagNames;
    /** この名前(部分一致)のタグが付いたら「面白さ」方向 (既定方向でもある)。 */
    std::vector<std::string> funTagNames;
};

const ForumDirectionConfig& DEFAULT_FORUM_DIRECTION_CONFIG();

/**
 * フォーラムポストの適用タグ名から議論の方向性を決める (純粋関数)。
 * 改善系タグがあれば "improvement"、面白さ系なら "fun"、どちらも無ければ既定 "fun"。
 */
ForumDirection pick_forum_direction(const std::vector<std::string>& applied_tag_names,
                                    const ForumDirectionConfig& cfg = DEFAULT_FORUM_DIRECTION_CONFIG());

struct DiscordScene {
    std::string guildId;
    std::string channelId;
};
/** scene 文字列 "discord:<guildId>/<channelId>" を分解。形式不正なら nullopt。 */
std::optional<DiscordScene> parse_discord_scene(const std::optional<std::string>& scene);

/** チャンネルがフォーラムスレッドか (= スレッドで、手元の親が GuildForum)。親が無ければ false。 */
bool is_forum_thread_channel(const ForumChannelRef* channel);

/** フォーラムポストの最初の投稿 (starter) か (開始メッセージ id はスレッド id と一致する)。 */
bool is_forum_starter_message(const std::string& message_id, const std::string& channel_id);

/** まとめ投稿チャンネル向けの収束まとめ整形。 */
std::string format_forum_summary(const std::string& title, const std::string& summary);

struct ForumMonitorDeps {
    /** route_forum_post に渡す共通 deps (workspaceId / classifyInboundMessage など)。 */
    CommandRouterDeps router;
    /** タグ→方向性の設定 (未設定なら既定マッピング)。 */
    std::optional<ForumDirectionConfig> directionConfig;
    /** starter 取得のリトライ待ち (既定 std::this_thread::sleep_for。テストで差し替える)。 */
    std::function<void(std::chrono::milliseconds)> sleep;
};

/** スレッドの適用タグ id を親フォーラムの availableTags でタグ名に解決する。 */
std::vector<std::string> thread_applied_tag_names(const Channel& thread, const std::optional<Channel>& parent_forum);

/**
 * フォーラムポスト新規作成 (ThreadCreate の d = スレッドの Channel JSON) を議論として起動する。
 * 親を取得して GuildForum でなければ何もしない。starter を取得 (作成直後は取りこぼすので 1 回リトライ) して
 * route_forum_post(isStarter=true) に流し、議論が立てば starter に 👀 を付ける。
 * 分類 (LLM) を同期で待つので、呼び出し側は背景のスレッドで呼ぶこと。失敗は warn して握りつぶす。
 */
void handle_forum_thread_create(const Json& thread, DiscordApi& api, const ForumMonitorDeps& deps);

/**
 * フォーラムスレッド内の後続投稿 (MessageCreate の d) を進行中議論へ取り込む。
 * 新規 gap は立てず utterance のみ記録 → event-bridge が人間発言としてミラーする。
 */
void handle_forum_reply(const Json& message, const ForumMonitorDeps& deps);

struct FinalizeForumPostArgs {
    /** 議論 session の scene ("discord:<guild>/<threadId>")。フォーラム以外は no-op。 */
    std::optional<std::string> scene;
    /** 収束まとめ本文 (【収束】プレフィックス除去済)。 */
    std::string summary;
    /** 議題タイトル。 */
    std::string title;
    /** まとめ転記先チャンネル名 (既定「まとめ投稿」)。 */
    std::optional<std::string> summaryChannelName;
    /** 自動作成カテゴリ名 (既定「システム」)。 */
    std::optional<std::string> categoryName;
};

struct FinalizeForumPostResult {
    bool closed = false;
    std::optional<std::string> reason;
};

/**
 * 収束したフォーラムポストを締める: まとめを「まとめ投稿」チャンネルへ転記し、スレッドを lock + archive する。
 * scene がフォーラムスレッドでない (通常チャンネル等) 場合は何もしない。
 */
FinalizeForumPostResult finalize_forum_post(DiscordApi& api, const FinalizeForumPostArgs& args);

} // namespace di::discord_hook
