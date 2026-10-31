#pragma once
// ログ。console.log / console.warn / console.error の代わり。
// 書き先: 標準出力・標準エラー (console と同じ) + 登録された sink
// (Vestigium 互換の JSONL ファイル、管理画面のエラーバッファなど)。
// 秘密値 (bot token・API キー・Authorization ヘッダ) を渡さないこと。
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace di::log {

enum class Level { trace, debug, info, warn, error, fatal };
std::string_view level_name(Level level);

struct Record {
    Level level;
    std::string message;
    std::int64_t ts_ms;
};

using Sink = std::function<void(const Record&)>;
/** sink を足す。戻り値の id で外せる。sink の中で log を呼ばないこと。 */
int add_sink(Sink sink);
void remove_sink(int id);

void write(Level level, std::string_view message);
inline void debug(std::string_view m) { write(Level::debug, m); }
inline void info(std::string_view m) { write(Level::info, m); }
inline void warn(std::string_view m) { write(Level::warn, m); }
inline void error(std::string_view m) { write(Level::error, m); }

/** 標準出力への複写を止める (テスト用)。 */
void set_console_enabled(bool enabled);

// ─── Vestigium 互換の JSONL 書き出し (@ludiars/vestigium の移植) ────────────
// 書式: {"ts":<ms>,"level":"info","service":"discutere","channel":"app","msg":"...","pid":<pid>}
// ファイル: <logsDir>/<service>/<YYYY-MM-DD>.jsonl (UTC 日付で日次切替)。msg は 64KiB で切る。
struct VestigiumOptions {
    std::string service_code;      // 小文字英数 + _ - 、64 文字まで
    std::string logs_dir;          // 空なら env VESTIGIUM_LOGS_DIR、無ければ ./logs
    int retention_days = 14;       // 0 で削除しない
    std::int64_t sweep_interval_ms = 60 * 60 * 1000;
};

class Vestigium {
public:
    explicit Vestigium(VestigiumOptions options);
    ~Vestigium();
    Vestigium(const Vestigium&) = delete;
    Vestigium& operator=(const Vestigium&) = delete;
    /** 保持期限を過ぎた日次ファイルを消す (今日のファイルは残す)。消したファイル数を返す。 */
    int sweep(std::int64_t now_ms);
    void shutdown();
    const std::string& logs_dir() const;
    std::string current_file(std::int64_t now_ms) const;

private:
    struct Impl;
    Impl* impl_;
};

/** vestigium の serialize と同じ 1 行 (末尾 \n 付き)。 */
std::string serialize_record(std::int64_t ts, Level level, std::string_view service, std::string_view channel,
                             std::string_view msg, std::int64_t pid);
/** service code の検査 (vestigium sanitizeCode)。不正は std::invalid_argument。 */
std::string sanitize_service_code(std::string_view code);

} // namespace di::log
