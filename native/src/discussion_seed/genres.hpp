#pragma once
// ジャンル別「何が面白い?」自動シード議論の種カタログ (#64)。移植元 src/discussion-seed/genres.ts。
// ars-game-lexicon の 17 ジャンルを Di 内に静的リストとして持つ (cross-repo path 依存を避ける)。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::discussion_seed {

struct GenreSeed {
    /** ジャンル名 (日本語) */
    std::string genre;
    /** 議論の切り口を補足する一言 (任意) */
    std::optional<std::string> angle;
};

struct SeedTopic {
    std::string title;
    std::string description;
};

Json to_json(const SeedTopic& t);

/** ars-game-lexicon 準拠の 17 ジャンル (静的フォールバック)。 */
const std::vector<GenreSeed>& GENRE_SEEDS();

/** ジャンル種 → 「何が面白い?」議題テキストに展開する。 */
SeedTopic build_genre_topic(const GenreSeed& seed);

/** index でジャンルを 1 つ選ぶ (決定的・負の index も巡回)。空リストは std::invalid_argument。 */
const GenreSeed& pick_genre_seed(std::int64_t index, const std::vector<GenreSeed>& seeds = GENRE_SEEDS());

} // namespace di::discussion_seed
