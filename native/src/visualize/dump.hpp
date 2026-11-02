#pragma once
// export_node の結果を `data/discussions/<workspace>/<type-dir>/<id>.md` に書き出す (src/visualize/dump.ts)。
// Phase 0 は depth=0 (隣接ノードは生成しない、wikilink だけ埋める)。
#include <optional>
#include <string>

#include "visualize/md_exporter.hpp"

namespace di::visualize {

struct DumpOptions {
    /** 出力 root (default: ./data/discussions) */
    std::optional<std::string> rootDir;
    std::string workspaceId;
};

struct DumpResult : ExportResult {
    std::string filePath;
    /** "created" | "updated" | "unchanged" */
    std::string written;
};

/**
 * ノードを md にして書き出す (内容が同じなら書かない)。
 * workspace / id が root の外を指す (".." や絶対パス) ときは std::invalid_argument (移植で追加した検査)。
 * 書き込み失敗は std::runtime_error。
 */
DumpResult dump_node(core::Core& core, const std::string& type, const std::string& id, const DumpOptions& options);

/** 出力先のパス。root の外に出る組み合わせは std::invalid_argument。 */
std::string resolve_file_path(const std::string& type, const std::string& id, const DumpOptions& options);

} // namespace di::visualize
