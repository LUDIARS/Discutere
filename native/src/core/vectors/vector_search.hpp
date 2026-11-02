#pragma once
// 埋め込みベクトルの検索 (src/core/vectors/vector-search.ts)。
// fetch_vectors_by_node_ids: キーワード検索で絞った候補だけをベクトル再ランクする二段構えの下半分。
// search_similar: workspace (と node_type) の全件を cos 類似度で並べる。
// 壊れた行・有限数ベクトルでない行は結果から外す (TS と同じ)。vector_json は上限つきで読む。
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "base/sqlite.hpp"

namespace di::core::vectors {

/** vector_json 1 行の上限 (bge-m3 の 1024 次元は 20KB 程度)。 */
inline constexpr JsonLimits kVectorJsonLimits{4u << 20, 200'000, 4};
/** 全件走査の行数・バイト数の上限。 */
inline constexpr sql::QueryLimits kVectorScanLimits{2'000'000, 1024u << 20};

struct FetchVectorsInput {
    std::string workspaceId;
    std::string nodeType;
    std::vector<std::string> nodeIds;
};

std::map<std::string, std::vector<double>> fetch_vectors_by_node_ids(sql::Database& db, const FetchVectorsInput& input);

struct SearchSimilarInput {
    std::string workspaceId;
    std::vector<double> vector;
    double k = 0;
    std::optional<std::string> nodeType;
};

struct SimilarNode {
    std::string nodeType;
    std::string nodeId;
    double score = 0;
};
Json to_json(const SimilarNode& v);

std::vector<SimilarNode> search_similar(sql::Database& db, const SearchSimilarInput& input);

/** vector_json を読む (壊れた・有限数ベクトルでない・大きすぎるものは nullopt)。 */
std::optional<std::vector<double>> parse_vector_json(const std::string& text);

} // namespace di::core::vectors
