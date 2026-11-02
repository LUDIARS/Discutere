#pragma once
// 学習データの tar.gz アーカイブ生成。移植元 src/backup/archive.ts。
// tar を自作しないため、作成そのものは Node のサイドカー (native/sidecar/backup-archive.ts) で行う。
// Discatier KG + persona-engine.db + discutere.db を 1 本の tar.gz にまとめる。
// 存在しないターゲットは skip し、最低 1 件あればアーカイブする。cwd 相対パスで格納する。
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::backup {

struct CreateArchiveArgs {
    /** アーカイブ対象 (ファイル or ディレクトリのパス) */
    std::vector<std::string> targets;
    /** 出力 tar.gz パス */
    std::string outFile;
    /** 相対パス基準 (既定 作業ディレクトリ) */
    std::optional<std::string> cwd;
};

struct CreateArchiveResult {
    std::string outFile;
    /** 実際に含めた cwd 相対パス */
    std::vector<std::string> included;
    /** 存在せず skip したターゲット */
    std::vector<std::string> skipped;
};

/** アーカイブ作成の期限 (KG は数百 MB になるので長め)。 */
inline constexpr std::chrono::minutes kArchiveTimeout{30};

Json to_json(const CreateArchiveResult& r);

/** 失敗 (対象が 1 件も無い・サイドカーの失敗・期限切れ) は std::runtime_error。 */
CreateArchiveResult create_archive(const CreateArchiveArgs& args);

} // namespace di::backup
