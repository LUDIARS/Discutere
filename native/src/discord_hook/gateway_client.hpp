#pragma once
// Discord Gateway v10 クライアント (discord.js の WebSocketManager / WebSocketShard の置き換え。TS に対応ファイルは無い)。
//
// discord.js が裏でやっていたことを明示的に書く:
// - 接続: GET /gateway/bot (DiscordApi) で URL と session_start_limit を得て `<url>/?v=10&encoding=json` に
//   di::ws::Client で接続する (圧縮なし)。本番は wss:// かつ discord.gg / discord.com 配下の host だけを許す。
// - Hello → heartbeat_interval で heartbeat (初回は interval × jitter)。次の heartbeat までに ACK (op 11) が
//   来なければゾンビ接続として切って resume。サーバからの Heartbeat 要求 (op 1) には即座に応答する。
// - Identify (token・intents・properties) / Resume (session_id・seq・resume_gateway_url)。
// - op 7 Reconnect → resume。op 9 Invalid Session: d=true なら resume、false なら 1〜5 秒待って identify。
// - close code: 4004 / 4010〜4014 は再接続しない (on_fatal)。4007 / 4009 / 1000 は session を捨てて identify。
//   それ以外 (通信断 1006 を含む) は resume。再接続は指数 backoff + jitter (上限 reconnect_max)。
// - 送信のレート制限: 60 秒 120 件 (heartbeat・identify・resume のために reserved 件を残す)。
//   identify は session_start_limit (remaining=0 なら reset_after まで待つ) と max_concurrency (5 秒あたり) を守る。
// - 重複排除: 同じ session 内で seq が戻った dispatch は捨てる。加えて MESSAGE_CREATE / INTERACTION_CREATE /
//   THREAD_CREATE / MESSAGE_REACTION_ADD は鍵 (id 等) を一定時間・一定件数の既読集合で覚え、2 度渡さない。
// - 受信の上限: 1 メッセージ max_message_bytes (Beast の read_message_max + lorica)、JSON は json_limits。
//   壊れた JSON のフレームは捨てて warn (接続は保つ)。上限超過は ws が 1009 で閉じ、resume で繋ぎ直す。
// - token はログ・エラー文に出さない (identify の本文はログしない)。
//
// スレッド: 状態機械は専用の制御スレッド 1 本で回す。ws の I/O スレッドは受信を制御スレッドの
// キューに積むだけ。ハンドラ (on_dispatch) は既定で executor (background_executor) に渡す。
// 満杯なら捨てて warn する (stats().dropped_queue_full)。executor に渡すのでイベント間の順序は保証しない。
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/executor.hpp"
#include "base/json.hpp"
#include "discord_hook/discord_api.hpp"

namespace di::discord_hook {

/** Gateway intents (discord.js GatewayIntentBits と同じ値)。 */
namespace gateway_intent {
inline constexpr std::uint64_t Guilds = 1ull << 0;
inline constexpr std::uint64_t GuildMembers = 1ull << 1;
inline constexpr std::uint64_t GuildMessages = 1ull << 9;
inline constexpr std::uint64_t GuildMessageReactions = 1ull << 10;
inline constexpr std::uint64_t DirectMessages = 1ull << 12;
inline constexpr std::uint64_t DirectMessageReactions = 1ull << 13;
inline constexpr std::uint64_t MessageContent = 1ull << 15;
} // namespace gateway_intent

/** gateway.ts の Client 生成と同じ intents (Guilds / GuildMessages / MessageContent / DirectMessages / GuildMessageReactions)。 */
inline constexpr std::uint64_t DEFAULT_GATEWAY_INTENTS = gateway_intent::Guilds | gateway_intent::GuildMessages |
                                                         gateway_intent::MessageContent | gateway_intent::DirectMessages |
                                                         gateway_intent::GuildMessageReactions;

/** Gateway opcode。 */
namespace gateway_op {
inline constexpr int Dispatch = 0;
inline constexpr int Heartbeat = 1;
inline constexpr int Identify = 2;
inline constexpr int PresenceUpdate = 3;
inline constexpr int VoiceStateUpdate = 4;
inline constexpr int Resume = 6;
inline constexpr int Reconnect = 7;
inline constexpr int RequestGuildMembers = 8;
inline constexpr int InvalidSession = 9;
inline constexpr int Hello = 10;
inline constexpr int HeartbeatAck = 11;
} // namespace gateway_op

/** close code を受けたときの扱い。 */
enum class GatewayCloseAction {
    Resume,      // session を保って resume (session が無ければ identify)
    Reidentify,  // session を捨てて identify
    Fatal,       // 再接続しない (認証失敗・intents 不正等)
};
GatewayCloseAction classify_close_code(int code);

/** "<base>/?v=10&encoding=json" を作る (既存のクエリ・末尾の "/" は置き換える)。 */
std::string build_gateway_url(std::string_view base);

/**
 * Gateway の URL として受け付けてよいか。本番 (allow_insecure_test_endpoints=false) は wss:// かつ
 * host が discord.gg / discord.com かその配下だけ。テストは ws:// と任意の host を許す。
 */
bool is_allowed_gateway_url(std::string_view url, bool allow_insecure_test_endpoints);

/**
 * 送信のレート制限 (window あたり limit 件)。priority (heartbeat / identify / resume) は limit まで、
 * それ以外は limit - reserved まで。時刻は呼び出し側が渡す (テストしやすいように)。
 */
class GatewaySendLimiter {
public:
    using Clock = std::chrono::steady_clock;
    GatewaySendLimiter(std::size_t limit, std::size_t reserved, std::chrono::milliseconds window);
    bool try_acquire(bool priority, Clock::time_point now);
    /** 次に 1 件空く時刻 (今空いていれば now)。 */
    Clock::time_point next_free(bool priority, Clock::time_point now);
    std::size_t used(Clock::time_point now);
    void reset();

private:
    void prune(Clock::time_point now);
    std::size_t limit_;
    std::size_t reserved_;
    std::chrono::milliseconds window_;
    std::deque<Clock::time_point> sent_;
};

/** 既読集合 (鍵を ttl の間・最大 capacity 件覚える。溢れたら古いものから忘れる)。 */
class GatewayDedupe {
public:
    using Clock = std::chrono::steady_clock;
    GatewayDedupe(std::size_t capacity, std::chrono::milliseconds ttl);
    /** 初めて見た鍵なら覚えて true、既読なら false。 */
    bool first_seen(const std::string& key, Clock::time_point now);
    void forget(const std::string& key);
    std::size_t size() const { return order_.size(); }

private:
    void prune(Clock::time_point now);
    std::size_t capacity_;
    std::chrono::milliseconds ttl_;
    std::deque<std::pair<Clock::time_point, std::string>> order_;
    std::unordered_map<std::string, Clock::time_point> seen_;
};

/**
 * dispatch の重複排除の鍵。対象外のイベント (MESSAGE_UPDATE 等、同じ id で正当に何度も来るもの) は nullopt。
 * MESSAGE_CREATE / INTERACTION_CREATE / THREAD_CREATE: "<event>:<d.id>"。
 * MESSAGE_REACTION_ADD: "MESSAGE_REACTION_ADD:<message_id>:<user_id>:<emoji.id か emoji.name>"
 * (MESSAGE_REACTION_REMOVE で同じ鍵を忘れる。付け直しは新しいイベントとして渡す)。
 */
std::optional<std::string> gateway_dedupe_key(std::string_view event, const Json& d);

struct GatewayClientOptions {
    /** bot token。ログ・エラー文に出さない。 */
    std::string token;
    std::uint64_t intents = DEFAULT_GATEWAY_INTENTS;
    /** identify の properties。 */
    std::string os = "linux";
    std::string browser = "discutere-native";
    std::string device = "discutere-native";
    /** GET /gateway/bot に使う REST の設定 (token が空なら上の token を使う)。Gateway は専用の DiscordApi を持つ。 */
    DiscordApiOptions rest;

    /** 受信した dispatch (event 名 = t, d)。READY / RESUMED も渡す。 */
    std::function<void(const std::string& event, const Json& d)> on_dispatch;
    /** 再接続しない close (4004 等) で止まったとき (理由。token は含まない)。 */
    std::function<void(int code, const std::string& reason)> on_fatal;
    /** true なら on_dispatch を executor で呼ぶ。false なら制御スレッドで直接呼ぶ (テスト用。重い処理は heartbeat を遅らせる)。 */
    bool dispatch_in_background = true;
    /** 背景実行先 (nullptr なら background_executor())。満杯なら捨てて warn。 */
    Executor* executor = nullptr;

    // ── 上限 ──
    std::size_t max_message_bytes = 8u << 20;
    JsonLimits json_limits{8u << 20, 500'000, 64};
    /** 1 送信の上限 (Discord は 4096 バイト)。 */
    std::size_t max_send_bytes = 4096;
    /** send() で積める件数。 */
    std::size_t max_send_queue = 64;
    std::size_t send_limit = 120;
    std::size_t send_reserved = 5;
    std::chrono::milliseconds send_window{60'000};
    std::size_t dedupe_capacity = 10'000;
    std::chrono::milliseconds dedupe_ttl{10 * 60'000};

    // ── 時間 ──
    std::chrono::milliseconds connect_timeout{15'000};
    /** 接続から Hello までの期限。 */
    std::chrono::milliseconds hello_timeout{20'000};
    /** Hello の heartbeat_interval の下限・上限 (壊れた値への防御)。 */
    std::chrono::milliseconds min_heartbeat_interval{50};
    std::chrono::milliseconds max_heartbeat_interval{5 * 60'000};
    std::chrono::milliseconds reconnect_base{1'000};
    std::chrono::milliseconds reconnect_max{60'000};
    /** op 9 (d=false) の後に identify までに待つ範囲 (Discord の推奨 1〜5 秒)。 */
    std::chrono::milliseconds invalid_session_wait_min{1'000};
    std::chrono::milliseconds invalid_session_wait_max{5'000};
    /** identify の max_concurrency の窓 (Discord は 5 秒)。 */
    std::chrono::milliseconds identify_window{5'000};
    /** session_start_limit の reset_after を待つ上限 (超えるなら上限ぶん待って取り直す)。 */
    std::chrono::milliseconds max_session_start_wait{24 * 60 * 60'000};
    /** stop() で close の応答を待つ時間。 */
    std::chrono::milliseconds close_grace{1'000};
    /** 接続に成功しない失敗がこの回数続いたら諦める (0 = 諦めない)。 */
    int max_consecutive_failures = 0;

    /** 偽サーバ (loopback・ws://・任意の host) を使うテストだけ true。 */
    bool allow_insecure_test_endpoints = false;
};

struct GatewayStats {
    std::uint64_t connects = 0;
    std::uint64_t identifies = 0;
    std::uint64_t resumes = 0;
    std::uint64_t heartbeats_sent = 0;
    std::uint64_t heartbeat_acks = 0;
    std::uint64_t zombie_reconnects = 0;
    std::uint64_t dispatched = 0;
    std::uint64_t duplicates_dropped = 0;
    std::uint64_t stale_seq_dropped = 0;
    std::uint64_t dropped_queue_full = 0;
    std::uint64_t malformed_frames = 0;
    std::int64_t last_latency_ms = -1;
    std::int64_t seq = -1;
    bool has_session = false;
};

class GatewayClient {
public:
    explicit GatewayClient(GatewayClientOptions options);
    ~GatewayClient();
    GatewayClient(const GatewayClient&) = delete;
    GatewayClient& operator=(const GatewayClient&) = delete;

    /** 制御スレッドを起こして接続を始める (2 度目以降は何もしない)。 */
    void start();
    /** 再接続を止め、接続を閉じ、制御スレッドを待つ (待ち・backoff 中でも戻る)。何度呼んでもよい。 */
    void stop();
    /** 制御スレッドが動いているか (fatal・諦め・stop の後は false)。 */
    bool running() const;
    /** READY / RESUMED の後で接続が開いているか。 */
    bool ready() const;
    /**
     * Gateway へ送る (presence update 等。op は 3/4/8 のみ)。接続 (ready) していない・キュー満杯・
     * 1 送信の上限超過なら false。レート制限の内側で順に送る。
     */
    bool send(int op, const Json& d);
    GatewayStats stats() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace di::discord_hook
