#include "core/kg_registry.hpp"

#include "base/fsutil.hpp"

namespace di::core {

namespace {
const char* const DEFAULT_KG_ID = "default";
}

Json to_json(const KnowledgeGraphEntry& v) {
    return Json{{"id", v.id}, {"label", v.label}, {"kuzuPath", v.kuzuPath}, {"active", v.active}};
}

std::optional<std::string> resolve_active_kg_path(const config::DiscutereConfig& config) {
    const auto& graphs = config.discatier.knowledgeGraphs;
    if (graphs && !graphs->empty()) {
        const std::string active_id = config.discatier.activeKg.value_or(DEFAULT_KG_ID);
        for (const auto& kg : *graphs)
            if (kg.id == active_id) return kg.kuzuPath;
        return graphs->front().kuzuPath;
    }
    // 旧 単一 kuzuPath (それも無ければ nullopt = 既定)。
    return config.discatier.kuzuPath;
}

std::string resolve_active_kg_dir(const config::DiscutereConfig& config) {
    const auto kg_path = resolve_active_kg_path(config);
    // TS の `if (!kgPath)` は空文字も未解決とみなす。
    if (!kg_path || kg_path->empty()) return fs::resolve("./data/external");
    return fs::dirname(fs::resolve(*kg_path));
}

std::vector<KnowledgeGraphEntry> list_knowledge_graphs(const config::DiscutereConfig& config) {
    std::vector<KnowledgeGraphEntry> out;
    const auto& graphs = config.discatier.knowledgeGraphs;
    if (graphs && !graphs->empty()) {
        const std::string active_id = config.discatier.activeKg.value_or(DEFAULT_KG_ID);
        bool has_match = false;
        for (const auto& kg : *graphs)
            if (kg.id == active_id) has_match = true;
        for (std::size_t i = 0; i < graphs->size(); ++i) {
            const auto& kg = (*graphs)[i];
            // 一致が無ければ先頭が実効 active (resolve_active_kg_path と整合)。
            out.push_back({kg.id, kg.label.value_or(kg.id), kg.kuzuPath, has_match ? kg.id == active_id : i == 0});
        }
        return out;
    }
    if (config.discatier.kuzuPath && !config.discatier.kuzuPath->empty())
        out.push_back({DEFAULT_KG_ID, "総合", *config.discatier.kuzuPath, true});
    return out;
}

} // namespace di::core
