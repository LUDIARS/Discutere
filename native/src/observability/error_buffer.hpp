#pragma once
// In-memory エラーリングバッファ (admin dashboard 可視化用。src/observability/error-buffer.ts の移植)。
//
// console-capture が拾った error / warn をここに積む。永続化はしない (プロセス内のみ /
// 上限 CAP 件で古いものから捨てる)。SQLite やファイルに残さないのは「直近の異常を素早く見る」用途に絞るため。
// 複数スレッドから積まれるので mutex で守る。
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"

namespace di::observability {

/** バッファの件数上限 (TS の CAP と同じ)。 */
inline constexpr std::size_t kErrorBufferCap = 200;
/** 1 件の message の上限 (UTF-16 単位)。TS には無いが、1 件で巨大な文字列を抱え込まないための上限。 */
inline constexpr std::size_t kErrorMessageMax = 64 * 1024;

struct BufferedError {
    std::int64_t ts = 0;
    std::string level;  // "error" | "warn"
    std::string source;
    std::string message;
};

Json to_json(const BufferedError& e);

/** 1 件記録する (古いものから CAP 超過分を捨てる)。 */
void record_error(std::string_view level, std::string_view source, std::string_view message);

/**
 * 直近のエラーを新しい順に返す (既定 200)。limit は JS の数値と同じ扱い
 * (NaN なら全件、小数は TS の slice と同じく切り捨て位置で数える)。
 */
std::vector<BufferedError> recent_errors(double limit = static_cast<double>(kErrorBufferCap));

/** バッファを空にする (dashboard の「クリア」)。 */
void clear_errors();

} // namespace di::observability
