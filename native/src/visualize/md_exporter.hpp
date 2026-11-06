#pragma once
// Discatier Core ノード → markdown レンダ (Phase 0, src/visualize/md-exporter.ts)。
//
// 各 export_xxx(core, ws, id) は frontmatter (YAML) + 本文 markdown を組み立てる。
// 参照は wikilink (`[[type:id]]`) で埋め込む。隣接ノードの md は生成しない (= depth=0)。traversal は dump。
// 失敗時は ExporterError を投げる (CLI 側で catch + exit code 1)。
// front matter の書き出しは gray-matter の matter.stringify と同じ (lib/front_matter::stringify)。
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "core/index.hpp"
#include "visualize/wikilink.hpp"

namespace di::visualize {

class ExporterError : public std::runtime_error {
public:
    ExporterError(const std::string& message, std::optional<std::string> type = std::nullopt, std::optional<std::string> id = std::nullopt)
        : std::runtime_error(message), type(std::move(type)), id(std::move(id)) {}
    std::optional<std::string> type;
    std::optional<std::string> id;
};

struct ExportResult {
    std::string type;
    std::string id;
    std::string markdown;
    /** 本ノードから出る wikilink (重複あり) — dump の traversal で使う */
    std::vector<Wikilink> outgoing;
};
Json to_json(const ExportResult& r);

ExportResult export_hypothesis(core::Core& core, const std::string& ws, const std::string& id);
ExportResult export_gap(core::Core& core, const std::string& ws, const std::string& id);
ExportResult export_mechanic(core::Core& core, const std::string& ws, const std::string& id);
ExportResult export_aesthetic(core::Core& core, const std::string& ws, const std::string& id);
ExportResult export_utterance(core::Core& core, const std::string& ws, const std::string& id);
ExportResult export_session(core::Core& core, const std::string& ws, const std::string& id);

using Exporter = ExportResult (*)(core::Core&, const std::string&, const std::string&);

/** EXPORTERS[type] (aff / game / persona は未対応 = nullptr)。未知の type も nullptr。 */
Exporter exporter_for(std::string_view type);

/** type の exporter で書き出す。未対応の type は ExporterError。 */
ExportResult export_node(core::Core& core, const std::string& type, const std::string& ws, const std::string& id);

} // namespace di::visualize
