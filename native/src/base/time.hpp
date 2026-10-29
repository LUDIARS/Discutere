#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace di::time {

/** Date.now() (UNIX エポックからのミリ秒)。 */
std::int64_t now_ms();
/** 単調時計のミリ秒 (経過時間の計測用)。 */
std::int64_t steady_ms();
/** new Date(ms).toISOString() → "2026-10-29T12:34:56.789Z" */
std::string iso_utc(std::int64_t ms);
std::string now_iso();
/** "YYYY-MM-DD" (UTC) */
std::string ymd_utc(std::int64_t ms);
/** SQLite の datetime('now') と同じ "YYYY-MM-DD HH:MM:SS" (UTC)。 */
std::string sqlite_datetime_utc(std::int64_t ms);
/**
 * ISO 8601 / SQLite 形式を読む。
 * "YYYY-MM-DD", "YYYY-MM-DDTHH:MM[:SS[.sss]][Z|±HH:MM]", "YYYY-MM-DD HH:MM:SS"。
 * タイムゾーン指定が無い日時は UTC として読む (JS は日付のみ=UTC / 日時=ローカルだが、
 * サーバの TZ に依存させないため UTC に揃える。各呼び出し側で必要なら補正する)。
 */
std::optional<std::int64_t> parse_iso(std::string_view text);
/** 年月日時分秒 (UTC) → エポックミリ秒。 */
std::int64_t from_civil_utc(int year, int month, int day, int hour = 0, int minute = 0, int second = 0, int millis = 0);
struct Civil {
    int year, month, day, hour, minute, second, millis;
};
Civil to_civil_utc(std::int64_t ms);

} // namespace di::time
