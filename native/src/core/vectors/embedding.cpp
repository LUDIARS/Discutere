#include "core/vectors/embedding.hpp"

#include <cmath>
#include <stdexcept>

#include "base/crypto.hpp"
#include "base/json.hpp"
#include "base/time.hpp"

namespace di::core::vectors {

void register_embedding(sql::Database& db, const RegisterEmbeddingInput& input) {
    bool ok = !input.vector.empty();
    for (double v : input.vector)
        if (!std::isfinite(v)) ok = false;
    if (!ok) throw std::invalid_argument("embedding vector must be a non-empty array of finite numbers");
    Json arr = Json::array();
    for (double v : input.vector) arr.push_back(v);
    db.run(R"SQL(INSERT INTO embeddings (id, workspace_id, node_type, node_id, vector_json, created_at)
     VALUES (?, ?, ?, ?, ?, ?)
     ON CONFLICT(workspace_id, node_type, node_id) DO UPDATE SET vector_json = excluded.vector_json, created_at = excluded.created_at)SQL",
           {crypto::uuid_v4(), input.workspaceId, input.nodeType, input.nodeId, json_stringify(arr), static_cast<double>(time::now_ms())});
}

} // namespace di::core::vectors
