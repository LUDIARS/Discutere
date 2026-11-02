#pragma once
// タスク別 KG レジストリの解決 (src/core/kg-registry.ts) — spec/feature/crawler/EXTERNAL-SOURCES.md §12。
//
// 解決順 (完全後方互換):
//   1. knowledgeGraphs があれば activeKg (既定 "default") に一致する id の kuzuPath。
//      一致が無く list が非空なら先頭にフォールバック。
//   2. 無ければ旧 `kuzuPath` 単独 (= id="default" の 1 KG とみなす)。
//   3. どちらも無ければ nullopt (KuzuClient の既定に倒す)。
// 無停止ホットスワップは非対応。切替は config の activeKg 変更 + 再起動。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "config.hpp"

namespace di::core {

struct KnowledgeGraphEntry {
    std::string id;
    std::string label;
    std::string kuzuPath;
    bool active = false;
};
Json to_json(const KnowledgeGraphEntry& v);

/** active KG の kuzu パス。該当が無ければ nullopt。 */
std::optional<std::string> resolve_active_kg_path(const config::DiscutereConfig& config);

/** sidecar (.ingested / .raw / .attribution) を置く active KG のディレクトリ (未解決なら ./data/external の絶対パス)。 */
std::string resolve_active_kg_dir(const config::DiscutereConfig& config);

/** 宣言済み KG 一覧 (active フラグ付き)。旧 単一 kuzuPath は default 1 件として見せる。 */
std::vector<KnowledgeGraphEntry> list_knowledge_graphs(const config::DiscutereConfig& config);

} // namespace di::core
