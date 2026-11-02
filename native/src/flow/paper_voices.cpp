#include "flow/paper_voices.hpp"

#include <algorithm>
#include <memory>

#include "base/strings.hpp"
#include "base/time.hpp"
#include "discatier_engine_adapter/keyword_terms.hpp"
#include "flow/db/connection.hpp"

namespace di::flow {

std::size_t save_paper_voices(const std::string& session_id, const std::vector<ContextVoice>& voices) {
    std::vector<const ContextVoice*> rows;
    for (const auto& v : voices) {
        if (str::trim(v.content).empty()) continue;
        rows.push_back(&v);
        if (rows.size() >= PAPER_VOICE_LIMIT) break;
    }
    if (rows.empty()) return 0;
    auto db = db::get_flow_db();
    const auto now = time::now_ms();
    db->transaction([&] {
        db->run("DELETE FROM discussion_paper_voice WHERE session_id = ?", {session_id});
        for (std::size_t i = 0; i < rows.size(); ++i)
            db->run(R"SQL(INSERT INTO discussion_paper_voice (session_id, ordinal, content, source, source_url, created_at)
     VALUES (?, ?, ?, ?, ?, ?))SQL",
                    {session_id, i, rows[i]->content, rows[i]->source, sql::Value(rows[i]->sourceUrl), now});
    });
    return rows.size();
}

std::vector<ContextVoice> load_paper_voices(const std::string& session_id) {
    const auto rows = db::get_flow_db()->all(
        "SELECT content, source, source_url FROM discussion_paper_voice WHERE session_id = ? ORDER BY ordinal", {session_id},
        sql::QueryLimits{PAPER_VOICE_LIMIT * 10, 64u << 20});
    std::vector<ContextVoice> out;
    for (const auto& r : rows) {
        ContextVoice v{r.str("content"), r.str("source"), std::nullopt};
        auto url = r.opt_str("source_url");
        if (url && !url->empty()) v.sourceUrl = *url;
        out.push_back(std::move(v));
    }
    return out;
}

VoiceLookup stored_voice_lookup(std::vector<ContextVoice> voices) {
    auto shared = std::make_shared<const std::vector<ContextVoice>>(std::move(voices));
    return [shared](const std::vector<std::string>& terms, int limit) {
        const auto keys = discatier_engine_adapter::extract_key_terms(terms);
        struct Scored {
            std::size_t index;
            std::size_t score;
        };
        std::vector<Scored> scored;
        for (std::size_t i = 0; i < shared->size(); ++i) {
            const std::string text = str::to_lower((*shared)[i].content);
            std::size_t score = 0;
            for (const auto& k : keys)
                if (str::contains(text, k)) ++score;
            scored.push_back({i, score});
        }
        std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
            return a.score != b.score ? a.score > b.score : a.index < b.index;
        });
        std::vector<ContextVoice> out;
        const std::size_t n = static_cast<std::size_t>(std::max(0, limit));
        for (std::size_t i = 0; i < scored.size() && i < n; ++i) out.push_back((*shared)[scored[i].index]);
        return out;
    };
}

VoiceLookup session_voice_lookup(const std::string& session_id, VoiceLookup fallback) {
    auto stored = load_paper_voices(session_id);
    return stored.empty() ? fallback : stored_voice_lookup(std::move(stored));
}

} // namespace di::flow
