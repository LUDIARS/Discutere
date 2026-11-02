#include "core/vectors/voice_index.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "base/strings.hpp"

namespace di::core::vectors {

std::size_t embed_voice_rows(Core& core, EmbeddingClient& embedder, const std::vector<VoiceRow>& rows, const EmbedVoiceOptions& opts) {
    std::vector<const VoiceRow*> targets;
    for (const auto& r : rows)
        if (!str::trim(r.raw_content.value_or("")).empty()) targets.push_back(&r);
    const std::size_t batch_size = static_cast<std::size_t>(std::max(1, opts.batchSize));
    std::size_t done = 0;
    for (std::size_t i = 0; i < targets.size(); i += batch_size) {
        const std::size_t end = std::min(targets.size(), i + batch_size);
        std::vector<std::string> texts;
        texts.reserve(end - i);
        for (std::size_t k = i; k < end; ++k)
            texts.push_back(str::js_slice(targets[k]->raw_content.value_or(""), 0, static_cast<std::int64_t>(EMBED_TEXT_CAP)));
        const auto vectors = embedder.embed(texts);
        if (vectors.size() < texts.size()) throw std::runtime_error("embedder returned fewer vectors than inputs");
        core.raw().transaction([&] {
            for (std::size_t k = i; k < end; ++k) {
                core.register_embedding(RegisterEmbeddingInput{opts.workspaceId, "utterance", targets[k]->id, vectors[k - i]});
            }
        });
        done += end - i;
        if (opts.onProgress) opts.onProgress(done, targets.size());
    }
    return done;
}

std::size_t embed_voices_by_ids(Core& core, EmbeddingClient& embedder, const std::vector<std::string>& ids, const EmbedVoiceOptions& opts) {
    if (ids.empty()) return 0;
    std::vector<VoiceRow> rows;
    std::set<std::string> seen;
    for (const auto& id : ids) {
        if (seen.size() >= kEmbedVoiceIdsMax) break;
        if (!seen.insert(id).second) continue;
        const auto row = core.raw().get(
            R"SQL(SELECT u.id, u.raw_content
       FROM utterances u
       LEFT JOIN embeddings e
         ON e.workspace_id = u.workspace_id AND e.node_type = 'utterance' AND e.node_id = u.id
      WHERE u.workspace_id = ? AND u.id = ? AND e.node_id IS NULL)SQL",
            {opts.workspaceId, id});
        if (row) rows.push_back(VoiceRow{row->str("id"), row->opt_str("raw_content")});
    }
    return embed_voice_rows(core, embedder, rows, opts);
}

} // namespace di::core::vectors
