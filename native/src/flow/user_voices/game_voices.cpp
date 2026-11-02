#include "flow/user_voices/game_voices.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "base/strings.hpp"
#include "core/vectors/vector_math.hpp"
#include "core/vectors/vector_search.hpp"

namespace di::flow::user_voices {

namespace {

/** 収集した声の出所 (Steam レビュー / GLAB 感想)。 */
const char* const USER_VOICE_SOURCES[] = {"steam", "glab"};

struct VoiceRowItem {
    std::string id;
    std::string content;
    std::string source;
    std::string sourceUrl;
    double at = 0;
};

} // namespace

std::vector<ContextVoice> list_game_voices(const ListGameVoicesArgs& args) {
    if (!args.core || !args.attribution) throw std::invalid_argument("list_game_voices requires core and attribution");
    std::vector<crawler::sources::Attribution> attributions;
    for (const char* source : USER_VOICE_SOURCES) {
        auto list = args.attribution->list_by_source(source, args.gameSlug, GAME_VOICE_CANDIDATE_CAP);
        for (auto& a : list) attributions.push_back(std::move(a));
    }
    if (attributions.empty()) return {};
    std::vector<VoiceRowItem> rows;
    for (const auto& a : attributions) {
        const auto row = args.core->raw().get("SELECT raw_content FROM utterances WHERE workspace_id = ? AND id = ?", {args.workspaceId, a.utteranceId});
        if (!row) continue;
        const auto raw = row->opt_str("raw_content");
        const std::string content = raw ? str::trim(*raw) : std::string();
        if (content.empty()) continue;
        rows.push_back(VoiceRowItem{a.utteranceId, content, a.source, a.sourceUrl, a.importedAt});
    }

    if (args.queryVector && !args.queryVector->empty()) {
        std::vector<std::string> ids;
        ids.reserve(rows.size());
        for (const auto& r : rows) ids.push_back(r.id);
        const auto vectors = core::vectors::fetch_vectors_by_node_ids(args.core->raw(), {args.workspaceId, "utterance", ids});
        const auto& q = *args.queryVector;
        // ベクトルのある声を近い順に先に、未ベクトル化の声はその後ろ (新しい順)。
        std::vector<std::pair<double, std::size_t>> scored;
        scored.reserve(rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto it = vectors.find(rows[i].id);
            double score = it != vectors.end() ? core::vectors::cosine_similarity(q, it->second) : -std::numeric_limits<double>::infinity();
            if (std::isnan(score)) score = -std::numeric_limits<double>::infinity();  // 並べ替えの比較を壊さない
            scored.emplace_back(score, i);
        }
        std::stable_sort(scored.begin(), scored.end(), [&](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return rows[a.second].at > rows[b.second].at;
        });
        std::vector<VoiceRowItem> ordered;
        ordered.reserve(rows.size());
        for (const auto& s : scored) ordered.push_back(std::move(rows[s.second]));
        rows = std::move(ordered);
    }
    std::vector<ContextVoice> out;
    const std::size_t limit = static_cast<std::size_t>(std::max(0, args.limit));
    for (std::size_t i = 0; i < rows.size() && i < limit; ++i) {
        ContextVoice v;
        v.content = rows[i].content;
        v.source = rows[i].source;
        if (!rows[i].sourceUrl.empty()) v.sourceUrl = rows[i].sourceUrl;
        out.push_back(std::move(v));
    }
    return out;
}

} // namespace di::flow::user_voices
