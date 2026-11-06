#pragma once
// Node のまま残す機能 (PDF/DOCX 抽出・Canalis のクロール・tar 作成) を別プロセスで呼ぶ。
//   node --import tsx <repo>/native/sidecar/<script> を引数配列で起動し (シェルを経由しない)、
//   標準入力に JSON を 1 つ渡し、標準出力の JSON ({"ok":true,"result":...} / {"ok":false,"error":"..."}) を読む。
// node の場所は env DISCUTERE_NODE (既定 "node")、リポのルートは env DISCUTERE_REPO_ROOT (既定 作業ディレクトリ)。
// 期限と出力上限を必ず課す。上限超過・期限切れ・壊れた出力は失敗として返す。
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "base/json.hpp"

namespace di::sidecar {

struct Options {
    std::chrono::milliseconds timeout{120'000};
    std::size_t max_output = 32u << 20;
    /** 出力 JSON の上限 (lorica で課金)。 */
    JsonLimits json_limits{32u << 20, 2'000'000, 64};
};

struct Result {
    bool ok = false;
    Json result;
    std::string error;
};

/** script は native/sidecar からの相対パス (例 "spec-extract.ts")。".." や絶対パスは拒否する。 */
Result run(std::string_view script, const Json& input, const Options& options = {});

/** リポのルート (DISCUTERE_REPO_ROOT か作業ディレクトリ)。 */
std::string repo_root();

} // namespace di::sidecar
