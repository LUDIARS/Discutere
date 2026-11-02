#include "discord_hook/reactions.hpp"

#include <algorithm>

#include "base/time.hpp"

namespace di::discord_hook {

const ReactionWeights& default_reaction_weights() {
    static const ReactionWeights w{{"👍", 1}, {"❤️", 2}, {"🔥", 2}, {"✅", 2}, {"🎯", 2}, {"💡", 3}, {"💯", 3}, {"⭐", 2}};
    return w;
}

int reaction_weight(const std::string& emoji, const ReactionWeights& weights) {
    auto it = weights.find(emoji);
    return it == weights.end() ? 1 : it->second;
}

void ensure_reaction_tables(sql::Database& db) {
    db.exec(
        "CREATE TABLE IF NOT EXISTS discord_message_map (\n"
        "       message_id TEXT PRIMARY KEY,\n"
        "       target_id TEXT NOT NULL,\n"
        "       target_kind TEXT NOT NULL,\n"
        "       channel_id TEXT,\n"
        "       created_at INTEGER NOT NULL\n"
        "     );\n"
        "     CREATE TABLE IF NOT EXISTS opinion_scores (\n"
        "       target_id TEXT PRIMARY KEY,\n"
        "       target_kind TEXT NOT NULL,\n"
        "       score INTEGER NOT NULL DEFAULT 0,\n"
        "       updated_at INTEGER NOT NULL\n"
        "     );");
}

void record_posted_message(sql::Database& db, const PostedMessageInput& input) {
    if (input.messageId.empty()) return;
    db.run("INSERT OR REPLACE INTO discord_message_map (message_id, target_id, target_kind, channel_id, created_at) VALUES (?, ?, ?, ?, ?)",
           {input.messageId, input.targetId, input.targetKind, sql::Value(input.channelId), input.at.value_or(time::now_ms())});
}

std::optional<AppliedReaction> apply_reaction(sql::Database& db, const ReactionInput& input) {
    auto row = db.get("SELECT target_id, target_kind FROM discord_message_map WHERE message_id = ?", {input.messageId});
    if (!row) return std::nullopt;
    const int weight = reaction_weight(input.emoji, input.weights ? *input.weights : default_reaction_weights());
    const std::string target_id = row->str("target_id");
    const std::string target_kind = row->str("target_kind");
    db.run(
        "INSERT INTO opinion_scores (target_id, target_kind, score, updated_at)\n"
        "       VALUES (?, ?, ?, ?)\n"
        "     ON CONFLICT(target_id) DO UPDATE SET score = score + excluded.score, updated_at = excluded.updated_at",
        {target_id, target_kind, weight, input.at.value_or(time::now_ms())});
    auto cur = db.get("SELECT score FROM opinion_scores WHERE target_id = ?", {target_id});
    return AppliedReaction{target_id, target_kind, weight, cur ? cur->i64("score", weight) : weight};
}

std::int64_t get_opinion_score(sql::Database& db, const std::string& target_id) {
    auto row = db.get("SELECT score FROM opinion_scores WHERE target_id = ?", {target_id});
    return row ? row->i64("score") : 0;
}

std::vector<ScoredOpinion> top_scored_opinions(sql::Database& db, const std::vector<std::string>& utterance_ids, std::size_t limit) {
    std::vector<ScoredOpinion> out;
    for (const auto& id : utterance_ids) {
        const auto s = get_opinion_score(db, id);
        if (s > 0) out.push_back({id, s});
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

} // namespace di::discord_hook
