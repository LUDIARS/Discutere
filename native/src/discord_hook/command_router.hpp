#pragma once
// Transport 非依存の Discord → Discatier ルーティング (src/discord-hook/command-router.ts の移植)。
//
// - route_slash_command: slash command (interaction) を処理し返信文を返す。
//   * discutere-kill / discutere-status / discutere-backup / persona-generate → admin allowlist
//     (CommandRouterDeps::adminIds)。**空なら admin コマンドは全拒否 (安全 default)**。
//   * それ以外 → Discatier の `/<name> <args>` に組み立て submit_message へ
// - route_inbound_message: 平文メッセージを discord-bound session の utterance に記録 (許可チャンネルのみ)。
// - route_forum_post: フォーラムポストの取り込み (starter なら議論の種を起こす)。
//
// 受信 (Gateway) は後の担当。入口は interaction / message の JSON を受ける関数
// (handle_slash_interaction / handle_inbound_message_json)。返信は SlashReplier (REST 実装は DiscordApi の
// interaction callback)。
//
// TS の seed (Promise<boolean>) は「呼べば分類して議論の種が立ったかを返す」SeedTask にした。呼び出し側は
// resolve_seed_in_background で背景のスレッドに渡し、true のときだけ 👀 を付ける。
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "backup/runner.hpp"
#include "base/json.hpp"
#include "core/index.hpp"
#include "discord_hook/auto_discussion.hpp"
#include "discord_hook/discord_api.hpp"
#include "discord_hook/types.hpp"
#include "persona_engine/index.hpp"

namespace di::discord_hook {

struct SlashReply {
    std::string content;
    bool ephemeral = true;
};

struct InboundSlashCommand {
    /** command 名 (例 "propose", "discutere-kill") */
    std::string name;
    /** option 値を空白連結したもの (例 "選択肢を絞る 詳細")。無ければ空文字 */
    std::string argsText;
    /** discutere-kill の enabled boolean option (あれば) */
    std::optional<bool> enabledOption;
    /** 発話者 Discord user id (DM/guild 双方) */
    std::optional<std::string> userId;
    /** guild id (DM なら "dm") */
    std::string guildId = "dm";
    /** channel id (不明なら "default") */
    std::string channelId = "default";
};

/** /economy-graph の起動結果 (グラフ URL の組み立てに使う)。 */
struct EconomyAnalysisStarted {
    std::string slug;
    int port = 0;
};

/** /persona-generate の結果。 */
struct PersonaGenerateResult {
    double adopted = 0;
    double speakers = 0;
};

/** 平文投稿から議題を自動検出して persona-engine の議論開始イベントにつなぐ (started = 議論の種が新規に立った)。 */
using ClassifyInboundMessage = std::function<StarterResult(const DiscordAutoDiscussionInput&)>;

struct CommandRouterDeps {
    std::string workspaceId;
    /** admin slash の認可 allowlist。空なら全 deny (安全 default) */
    std::vector<std::string> adminIds;
    /**
     * 平文メッセージを utterance 取り込みする議論チャンネル id の許可リスト。
     * 空なら取り込まない (= 無関係チャンネルのノイズ混入を防ぐ安全 default)。
     */
    std::vector<std::string> discussionChannelIds;
    /** persona-engine の取得 (起動後に注入される singleton 想定)。未設定・nullptr は未初期化。 */
    std::function<std::shared_ptr<persona_engine::PersonaEngineHandle>()> getEngine;
    /** /discutere-queue 用の議論キューサマリ生成 (未設定なら非対応応答) */
    std::function<std::string()> buildQueueText;
    /** /discutere-backup 用の手動バックアップトリガ (未設定なら非対応応答)。背景のスレッドで呼ぶ。 */
    std::function<backup::BackupResult()> triggerBackup;
    /** /economy-graph 用の LLM 分析トリガ (fire-and-forget)。グラフ URL を返す */
    std::function<EconomyAnalysisStarted(const std::string& gameTitle)> triggerEconomyAnalysis;
    /** /persona-generate 用の憑依対象ペルソナ生成トリガ (item7)。背景のスレッドで呼ぶ。 */
    std::function<PersonaGenerateResult(const std::optional<std::string>& source)> triggerPersonaGenerate;
    /** 平文投稿から議題を自動検出して議論開始につなぐ (未設定なら seed を返さない)。 */
    ClassifyInboundMessage classifyInboundMessage;
    /** active KG を開く (既定は config の active KG)。 */
    std::function<std::unique_ptr<core::Core>()> openCore;
    /** fire-and-forget の仕事を積む (既定 background_executor().submit)。false = 満杯で積めなかった。 */
    std::function<bool(std::function<void()>)> submitBackground;
};

/** 呼べば分類器を走らせて「議論の種が立ったか」を返す (TS の seed Promise<boolean>)。例外は投げない (warn して false)。 */
using SeedTask = std::function<bool()>;

/** route_inbound_message / route_forum_post の結果。 */
struct InboundRouteResult {
    /** utterance に取り込んだか */
    bool ingested = false;
    /** 取り込んだ投稿が議論の種になったかを解決する仕事。分類器未設定・未取込・seed 不要なら空。 */
    SeedTask seed;
};

SlashReply route_slash_command(const InboundSlashCommand& cmd, const CommandRouterDeps& deps);

/**
 * 平文メッセージ (slash でない発話) を discord-bound session の utterance に記録する。
 * 議論チャンネル (または親が議論チャンネルのスレッド) のみ。bot 自身のメッセージは caller 側で除外する前提。
 */
InboundRouteResult route_inbound_message(const DiscordInboundMessage& msg, const std::string& guild_id, const CommandRouterDeps& deps,
                                         const std::optional<std::string>& parent_channel_id = std::nullopt);

struct ForumPostOptions {
    bool isStarter = false;
    std::optional<ForumDirection> direction;
    std::optional<std::string> forumTitle;
    std::optional<std::vector<std::string>> forumTags;
};

/**
 * フォーラムポストの取り込み (フォーラム集約)。discussionChannelIds の許可ゲートに依らない
 * (受信の境界は Di 管理フォーラムの forum_event_gate / managed_forum_scope が担う)。
 * isStarter かつ当該スレッドに open な議論が無いときだけ seed を返す。
 */
InboundRouteResult route_forum_post(const DiscordInboundMessage& msg, const std::string& guild_id, const CommandRouterDeps& deps,
                                    const ForumPostOptions& opts);

// ─── 入口 (Gateway 担当が受信 JSON を渡す) ─────────────────────────────

/** interaction の option 値を argsText 用の文字列に (string はそのまま、null は ""、他は JSON.stringify)。 */
std::string stringify_option_value(const Json& value);

/** INTERACTION_CREATE の d (APPLICATION_COMMAND) から InboundSlashCommand を作る。形が違えば std::runtime_error。 */
InboundSlashCommand inbound_slash_command_from_json(const Json& interaction);

/** slash の返信口 (interaction.reply({content, flags})). */
class SlashReplier {
public:
    virtual ~SlashReplier() = default;
    virtual void reply(const SlashReply& reply) = 0;
};

/** DiscordApi の interaction callback (type 4) で返す実装。メンションは解決しない ({parse: []})。 */
class RestSlashReplier : public SlashReplier {
public:
    RestSlashReplier(DiscordApi& api, std::string interaction_id, std::string interaction_token)
        : api_(api), id_(std::move(interaction_id)), token_(std::move(interaction_token)) {}
    void reply(const SlashReply& reply) override;

private:
    DiscordApi& api_;
    std::string id_;
    std::string token_;  // 秘密 (interaction token)。ログに出さない。
};

/** interaction callback の本文 ({type: 4, data: {content, flags?, allowed_mentions}})。 */
Json slash_reply_body(const SlashReply& reply);

/**
 * slash interaction (JSON) を route_slash_command に通して返信する。
 * route が例外を出したら "⚠️ internal error" (ephemeral) を返す (内容はログにも出さない)。
 * interaction の形が違えば返信せず false。
 */
bool handle_slash_interaction(const Json& interaction, const CommandRouterDeps& deps, SlashReplier& replier);

/** MESSAGE_CREATE の payload を正規化して route_inbound_message に通す (正規化できなければ ingested=false)。 */
InboundRouteResult handle_inbound_message_json(const Json& payload, const std::string& guild_id, const CommandRouterDeps& deps,
                                               const std::optional<std::string>& parent_channel_id = std::nullopt);

/**
 * seed を背景のスレッドで解決し、on_resolved(started) を呼ぶ (TS の seed.then)。
 * キューが満杯なら warn して on_resolved(false) (議論は立てない = 縮退)。seed が空なら何もしない。
 */
void resolve_seed_in_background(const SeedTask& seed, std::function<void(bool started)> on_resolved,
                                const std::function<bool(std::function<void()>)>& submit = nullptr);

// ─── 登録 (command_defs) との一致 ──────────────────────────────────────

/** route_slash_command が専用 handler で処理する名前。 */
const std::vector<std::string>& ROUTER_SLASH_COMMANDS();
/** gateway が専用の非同期 handler で処理する名前 (route に来たら保護応答 / submit_message)。 */
const std::vector<std::string>& DEDICATED_SLASH_COMMANDS();
/** submit_message (Discatier の /command) が処理する名前。 */
const std::vector<std::string>& DISCATIER_COMMANDS();
/** route_slash_command の admin allowlist を見る名前。 */
const std::vector<std::string>& ADMIN_SLASH_COMMANDS();

// ─── 整形 (テスト用に公開) ──────────────────────────────────────────

/** 連続改行を 1 つにして max (UTF-16) を超えたら max-1 文字 + "…"。 */
std::string truncate_text(std::string_view value, std::int64_t max);
/** new Date(ms).toLocaleString("ja-JP") (ローカル時刻 "YYYY/M/D H:MM:SS")。 */
std::string ja_locale_string(double ms);

} // namespace di::discord_hook
