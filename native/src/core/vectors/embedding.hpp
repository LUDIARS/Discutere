#pragma once
// 埋め込みベクトルの登録 (src/core/vectors/embedding.ts)。(workspace, node_type, node_id) ごとに 1 行を upsert する。
#include <string>
#include <vector>

#include "base/sqlite.hpp"

namespace di::core::vectors {

struct RegisterEmbeddingInput {
    std::string workspaceId;
    std::string nodeType;
    std::string nodeId;
    std::vector<double> vector;
};

/** 空・非有限数を含むベクトルは std::invalid_argument (TS と同じ文言)。 */
void register_embedding(sql::Database& db, const RegisterEmbeddingInput& input);

} // namespace di::core::vectors
