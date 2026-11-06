#include "core/vectors/vector_search.hpp"

#include <algorithm>
#include <cmath>

#include "core/vectors/vector_math.hpp"

namespace di::core::vectors {

std::optional<std::vector<double>> parse_vector_json(const std::string& text) {
    auto parsed = parse_json(text, kVectorJsonLimits);
    if (!parsed) return std::nullopt;
    return to_embedding_vector(*parsed);
}

std::map<std::string, std::vector<double>> fetch_vectors_by_node_ids(sql::Database& db, const FetchVectorsInput& input) {
    std::map<std::string, std::vector<double>> out;
    constexpr std::size_t CHUNK = 500;  // SQLite のバインド変数上限 (999) を割らないよう分割。
    for (std::size_t i = 0; i < input.nodeIds.size(); i += CHUNK) {
        const std::size_t end = std::min(input.nodeIds.size(), i + CHUNK);
        std::string placeholders;
        sql::Params params{input.workspaceId, input.nodeType};
        for (std::size_t j = i; j < end; ++j) {
            placeholders += j == i ? "?" : ",?";
            params.add(input.nodeIds[j]);
        }
        const auto rows = db.all(
            "SELECT node_id, vector_json FROM embeddings\n          WHERE workspace_id = ? AND node_type = ? AND node_id IN (" + placeholders + ")",
            params, kVectorScanLimits);
        for (const auto& r : rows) {
            // 壊れた行は rerank から外すだけ (再構築は build スクリプトの仕事)。
            if (auto v = parse_vector_json(r.str("vector_json"))) out[r.str("node_id")] = std::move(*v);
        }
    }
    return out;
}

Json to_json(const SimilarNode& v) { return Json{{"nodeType", v.nodeType}, {"nodeId", v.nodeId}, {"score", v.score}}; }

std::vector<SimilarNode> search_similar(sql::Database& db, const SearchSimilarInput& input) {
    const auto rows = input.nodeType && !input.nodeType->empty()
        ? db.all("SELECT node_type, node_id, vector_json FROM embeddings WHERE workspace_id = ? AND node_type = ?",
                 {input.workspaceId, *input.nodeType}, kVectorScanLimits)
        : db.all("SELECT node_type, node_id, vector_json FROM embeddings WHERE workspace_id = ?", {input.workspaceId},
                 kVectorScanLimits);
    std::vector<SimilarNode> out;
    for (const auto& r : rows) {
        auto v = parse_vector_json(r.str("vector_json"));
        if (!v) continue;
        out.push_back(SimilarNode{r.str("node_type"), r.str("node_id"), cosine_similarity(input.vector, *v)});
    }
    // Array#sort は安定 (b.score - a.score)。
    std::stable_sort(out.begin(), out.end(), [](const SimilarNode& a, const SimilarNode& b) { return a.score > b.score; });
    // slice(0, Math.max(0, k)): 小数は切り捨て、NaN は 0。
    double k = std::isnan(input.k) ? 0 : std::max(0.0, input.k);
    const std::size_t n = k >= static_cast<double>(out.size()) ? out.size() : static_cast<std::size_t>(std::trunc(k));
    out.resize(n);
    return out;
}

} // namespace di::core::vectors
