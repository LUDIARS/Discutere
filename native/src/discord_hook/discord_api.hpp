#pragma once
// Discord REST API v10 クライアント (discord.js の REST 部分 @discordjs/rest の置き換え。TS に対応ファイルは無い)。
//
// - base URL `https://discord.com/api/v10` (テストでは偽サーバへ差し替える)、`Authorization: Bot <token>`、
//   User-Agent `DiscordBot (https://github.com/LUDIARS/Discutere, 0.1)`。
// - レート制限: 応答ヘッダ X-RateLimit-Bucket / Remaining / Reset-After でバケットを管理し、残り 0 なら
//   リセットまで待ってから送る。429 は本文の retry_after (無ければ Retry-After ヘッダ) と global を尊重して待ち、
//   再試行する。待ちが max_rate_limit_wait を超えるなら待たずに失敗 (DiscordApiError, rate_limited=true)。
//   同じバケット (未判明ならルート) への要求は 1 本ずつ順に送る (discord.js の SequentialHandler と同じ)。
// - 再試行: 429 は max_retries 回まで。5xx と通信失敗は「冪等なメソッド (GET/PUT/PATCH/DELETE)」だけ再試行する
//   (discord.js は POST も再試行していたが、メッセージの二重投稿を避けるため POST は再試行しない)。
// - 期限 (1 要求ごと timeout)・応答本文の上限 (max_response_bytes)・JSON の上限 (json_limits)・同時実行の上限 (max_parallel)。
// - token はログ・エラー文に出さない。エラー文のルートは webhook / interaction の token を ":token" に伏せる。
// - キャッシュは持たない (discord.js の guild.channels.cache 等は使わず、必要なときに REST で取りに行く)。
//   例外として自分のユーザー (GET /users/@me) だけは一度取ったら保持する。
//
// discord.js のオブジェクト (Guild/Channel/Message/...) は、必要なフィールドだけを持つ struct と
// Discord API の JSON の形との変換 (from_json / to_json) で表す。
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.hpp"
#include "infra/http_client.hpp"

namespace di::discord_hook {

inline constexpr const char* DISCORD_API_BASE = "https://discord.com/api/v10";
inline constexpr const char* DISCORD_USER_AGENT = "DiscordBot (https://github.com/LUDIARS/Discutere, 0.1)";

/** discord.js ChannelType と同じ値。 */
namespace channel_type {
inline constexpr int GuildText = 0;
inline constexpr int DM = 1;
inline constexpr int GuildVoice = 2;
inline constexpr int GroupDM = 3;
inline constexpr int GuildCategory = 4;
inline constexpr int GuildAnnouncement = 5;
inline constexpr int AnnouncementThread = 10;
inline constexpr int PublicThread = 11;
inline constexpr int PrivateThread = 12;
inline constexpr int GuildStageVoice = 13;
inline constexpr int GuildDirectory = 14;
inline constexpr int GuildForum = 15;
inline constexpr int GuildMedia = 16;
} // namespace channel_type

/** スレッドの種類 (discord.js の channel.isThread())。 */
bool is_thread_type(int type);

/** discord.js PermissionFlagsBits の一部。 */
namespace permission {
inline constexpr std::uint64_t CreateInstantInvite = 1ull << 0;
inline constexpr std::uint64_t Administrator = 1ull << 3;
inline constexpr std::uint64_t ManageChannels = 1ull << 4;
inline constexpr std::uint64_t ManageGuild = 1ull << 5;
inline constexpr std::uint64_t AddReactions = 1ull << 6;
inline constexpr std::uint64_t ViewChannel = 1ull << 10;
inline constexpr std::uint64_t SendMessages = 1ull << 11;
inline constexpr std::uint64_t ManageMessages = 1ull << 13;
inline constexpr std::uint64_t ReadMessageHistory = 1ull << 16;
inline constexpr std::uint64_t ManageWebhooks = 1ull << 29;
inline constexpr std::uint64_t ManageThreads = 1ull << 34;
inline constexpr std::uint64_t SendMessagesInThreads = 1ull << 38;
inline constexpr std::uint64_t All = ~0ull;
} // namespace permission

// ─── API のオブジェクト (必要なフィールドだけ) ───────────────────────────

struct User {
    std::string id;
    std::string username;
    std::optional<std::string> global_name;
    bool bot = false;
};

struct ForumTag {
    std::optional<std::string> id;
    std::string name;
    std::optional<bool> moderated;
    std::optional<std::string> emoji_id;
    std::optional<std::string> emoji_name;
};

struct ThreadMetadata {
    bool archived = false;
    bool locked = false;
    std::optional<int> auto_archive_duration;
};

struct Channel {
    std::string id;
    int type = channel_type::GuildText;
    std::optional<std::string> guild_id;
    std::optional<std::string> name;
    std::optional<std::string> parent_id;
    std::optional<std::string> topic;
    std::optional<int> position;
    std::optional<std::string> owner_id;
    std::optional<std::string> last_message_id;
    std::vector<ForumTag> available_tags;
    std::vector<std::string> applied_tags;
    std::optional<ThreadMetadata> thread_metadata;
    bool is_thread() const { return is_thread_type(type); }
};

struct Role {
    std::string id;
    std::string name;
    std::uint64_t permissions = 0;
    int position = 0;
};

struct Guild {
    std::string id;
    std::string name;
    std::optional<std::string> owner_id;
    std::vector<Role> roles;
};

struct Member {
    std::optional<User> user;
    std::optional<std::string> nick;
    std::vector<std::string> roles;
};

struct MessageReference {
    std::optional<std::string> message_id;
    std::optional<std::string> channel_id;
    std::optional<std::string> guild_id;
};

struct Attachment {
    std::string id;
    std::string filename;
    std::string url;
    std::int64_t size = 0;
    std::optional<std::string> content_type;
};

struct Message {
    std::string id;
    std::string channel_id;
    std::optional<std::string> guild_id;
    User author;
    std::string content;
    std::string timestamp;
    std::vector<User> mentions;
    std::optional<MessageReference> message_reference;
    std::optional<std::string> webhook_id;
    std::vector<Attachment> attachments;
    int type = 0;
};

struct Webhook {
    std::string id;
    int type = 1;
    std::optional<std::string> name;
    std::optional<std::string> token;
    std::optional<std::string> channel_id;
    std::optional<std::string> guild_id;
};

struct GatewayBot {
    std::string url;
    int shards = 1;
    int session_start_total = 0;
    int session_start_remaining = 0;
    std::int64_t session_start_reset_after = 0;
    int max_concurrency = 1;
};

// JSON の形は Discord API のまま (snake_case)。不正な形は std::runtime_error。
User user_from_json(const Json& j);
Json to_json(const User& v);
ForumTag forum_tag_from_json(const Json& j);
/** PATCH/POST に送る形 ({id?, name, moderated?, emoji_id?, emoji_name?})。 */
Json to_json(const ForumTag& v);
Channel channel_from_json(const Json& j);
Json to_json(const Channel& v);
Role role_from_json(const Json& j);
Guild guild_from_json(const Json& j);
Member member_from_json(const Json& j);
Message message_from_json(const Json& j);
Json to_json(const Message& v);
Webhook webhook_from_json(const Json& j);
GatewayBot gateway_bot_from_json(const Json& j);

/** {"parse": []} (メンションを一切解決しない)。 */
Json allowed_mentions_none();

/** 添付ファイル (multipart/form-data の files[n])。 */
struct FileUpload {
    std::string name;
    std::string data;
    std::string content_type = "application/octet-stream";
};

// ─── クライアント ──────────────────────────────────────────────────

struct DiscordApiOptions {
    /** bot token。ログ・エラー文に出さない。 */
    std::string token;
    std::string base_url = DISCORD_API_BASE;
    std::string user_agent = DISCORD_USER_AGENT;
    std::chrono::milliseconds timeout{15'000};
    std::chrono::milliseconds connect_timeout{10'000};
    std::size_t max_response_bytes = 8u << 20;
    JsonLimits json_limits{8u << 20, 500'000, 64};
    /** レート制限で 1 回に待ってよい最大時間。これを超える待ちは失敗にする。 */
    std::chrono::milliseconds max_rate_limit_wait{30'000};
    /** 429 / 5xx / 通信失敗の再試行回数 (5xx・通信失敗は冪等なメソッドだけ)。 */
    int max_retries = 3;
    /** このクライアントからの同時要求数の上限 (1..64)。 */
    std::size_t max_parallel = 8;
    /** 偽サーバ (loopback) を使うテストだけ true。本番は Discord (公開アドレス) なので false。 */
    bool allow_private_network = false;
};

class DiscordApiError : public std::runtime_error {
public:
    DiscordApiError(const std::string& what, int status, int code, std::string route, bool rate_limited)
        : std::runtime_error(what), status_(status), code_(code), route_(std::move(route)), rate_limited_(rate_limited) {}
    /** HTTP status (通信失敗は 0)。 */
    int status() const { return status_; }
    /** Discord の JSON エラーコード (例 50013 = Missing Permissions)。無ければ 0。 */
    int code() const { return code_; }
    /** token を伏せたルート ("POST /channels/123/messages")。 */
    const std::string& route() const { return route_; }
    bool rate_limited() const { return rate_limited_; }

private:
    int status_;
    int code_;
    std::string route_;
    bool rate_limited_;
};

struct RequestOptions {
    std::vector<std::pair<std::string, std::string>> query;
    /** X-Audit-Log-Reason (URL エンコードして送る)。 */
    std::optional<std::string> reason;
    /** false なら Authorization を付けない (webhook 実行・interaction 応答は URL の token で認証する)。 */
    bool auth = true;
    /** 添付ファイル。あれば multipart/form-data (payload_json + files[n]) で送る。 */
    std::vector<FileUpload> files;
};

class DiscordApi {
public:
    explicit DiscordApi(DiscordApiOptions options);
    ~DiscordApi();
    DiscordApi(const DiscordApi&) = delete;
    DiscordApi& operator=(const DiscordApi&) = delete;

    /**
     * 汎用の要求。path は "/channels/123/messages" のように base_url からの相対。
     * 2xx は本文の JSON (204・空本文は null)。それ以外・壊れた応答・待ち上限超過は DiscordApiError。
     */
    Json request(std::string_view method, std::string_view path, const std::optional<Json>& body = std::nullopt,
                 const RequestOptions& options = {});

    // ── よく使う API ──
    /** GET /users/@me (一度取ったら保持)。 */
    User get_current_user();
    Guild get_guild(const std::string& guild_id);
    std::vector<Channel> get_guild_channels(const std::string& guild_id);
    Channel get_channel(const std::string& channel_id);
    /** POST /guilds/{id}/channels。body は Discord の形 ({name, type, parent_id?, topic?, available_tags?})。 */
    Channel create_guild_channel(const std::string& guild_id, const Json& body, const std::optional<std::string>& reason = std::nullopt);
    /** PATCH /channels/{id} (タグ・lock・archive 等)。 */
    Channel modify_channel(const std::string& channel_id, const Json& patch, const std::optional<std::string>& reason = std::nullopt);
    Member get_guild_member(const std::string& guild_id, const std::string& user_id);
    Message get_message(const std::string& channel_id, const std::string& message_id);
    Message create_message(const std::string& channel_id, const Json& body, const std::vector<FileUpload>& files = {});
    Message edit_message(const std::string& channel_id, const std::string& message_id, const Json& body);
    /** PUT /channels/{c}/messages/{m}/reactions/{emoji}/@me (emoji は Unicode か name:id、URL エンコードする)。 */
    void create_reaction(const std::string& channel_id, const std::string& message_id, const std::string& emoji);
    std::vector<Webhook> get_channel_webhooks(const std::string& channel_id);
    Webhook create_webhook(const std::string& channel_id, const std::string& name);
    /** POST /webhooks/{id}/{token}?wait=..&thread_id=.. (Authorization なし)。wait=false なら nullopt。 */
    std::optional<Message> execute_webhook(const std::string& webhook_id, const std::string& webhook_token, const Json& body,
                                           const std::optional<std::string>& thread_id = std::nullopt, bool wait = true);
    /** POST /interactions/{id}/{token}/callback (Authorization なし)。 */
    void create_interaction_response(const std::string& interaction_id, const std::string& interaction_token, const Json& body,
                                     const std::vector<FileUpload>& files = {});
    /** PATCH /webhooks/{application_id}/{token}/messages/@original */
    Message edit_original_interaction_response(const std::string& application_id, const std::string& interaction_token, const Json& body,
                                               const std::vector<FileUpload>& files = {});
    /** POST /webhooks/{application_id}/{token} (followup)。 */
    Message create_followup_message(const std::string& application_id, const std::string& interaction_token, const Json& body,
                                    const std::vector<FileUpload>& files = {});
    /** GET /gateway/bot */
    GatewayBot get_gateway_bot();
    /** PUT /applications/{app}/commands か /applications/{app}/guilds/{g}/commands。 */
    Json bulk_overwrite_commands(const std::string& application_id, const std::optional<std::string>& guild_id, const Json& commands);
    /** 自分 (bot) の guild での基本権限 (owner / Administrator は All)。 */
    std::uint64_t current_member_permissions(const std::string& guild_id);

    /** 以後の待ちを打ち切る (待っている要求は DiscordApiError で戻る)。 */
    void shutdown();
    const DiscordApiOptions& options() const { return options_; }

    struct Impl;

private:
    DiscordApiOptions options_;
    std::unique_ptr<Impl> impl_;
};

/** レート制限のバケットを分けるルートの鍵 ("GET /channels/123/messages/:id")。major parameter (channels/guilds/webhooks の id) は残す。 */
std::string rate_limit_route_key(std::string_view method, std::string_view path);
/** ログ・エラー文に出すルート (webhook / interaction の token を ":token" に伏せ、クエリを落とす)。 */
std::string redact_route(std::string_view method, std::string_view path);
/** Discord の permissions 文字列 (10 進) を読む。不正は 0。 */
std::uint64_t parse_permissions(const Json& value);
/** guild の roles と member の roles から基本権限を計算する (owner / Administrator は All)。 */
std::uint64_t compute_base_permissions(const Guild& guild, const Member& member, const std::string& user_id);

} // namespace di::discord_hook
