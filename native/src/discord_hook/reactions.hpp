#pragma once
// 議論意見へのリアクション → 内部スコアリング (src/discord-hook/reactions.ts の移植)。
//
// Discord に投稿した persona 発話 / 仮説の message_id ↔ 対象 を記録し、その message にリアクションが
// 付いたら絵文字ごとの重みで対象のスコアを加算する。テーブルは KG (SQLite) に同居させる。
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/sqlite.hpp"

namespace di::discord_hook {

// OpinionKind: "utterance" | "hypothesis"
using ReactionWeights = std::map<std::string, int>;

/** 絵文字ごとの加点重み (既定)。未知の絵文字は 1。 */
const ReactionWeights& default_reaction_weights();
int reaction_weight(const std::string& emoji, const ReactionWeights& weights = default_reaction_weights());

void ensure_reaction_tables(sql::Database& db);

struct PostedMessageInput {
    std::string messageId;
    std::string targetId;
    std::string targetKind;
    std::optional<std::string> channelId;
    std::optional<std::int64_t> at;
};
/** 投稿した Discord message と対象意見の対応を記録 (messageId 空は何もしない)。 */
void record_posted_message(sql::Database& db, const PostedMessageInput& input);

struct ReactionInput {
    std::string messageId;
    std::string emoji;
    std::optional<ReactionWeights> weights;
    std::optional<std::int64_t> at;
};
struct AppliedReaction {
    std::string targetId;
    std::string targetKind;
    int weight = 0;
    std::int64_t score = 0;
};
/** リアクションを 1 件適用してスコア加算。対象が見つからなければ nullopt。 */
std::optional<AppliedReaction> apply_reaction(sql::Database& db, const ReactionInput& input);

std::int64_t get_opinion_score(sql::Database& db, const std::string& target_id);

struct ScoredOpinion {
    std::string targetId;
    std::int64_t score = 0;
};
/** 高スコア意見を上位から (0 は除外、limit 件)。 */
std::vector<ScoredOpinion> top_scored_opinions(sql::Database& db, const std::vector<std::string>& utterance_ids, std::size_t limit);

} // namespace di::discord_hook
