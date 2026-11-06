#pragma once
// Crawler KG types (Phase 0) — src/crawler/types.ts の移植。
// data/games/<slug>.md の frontmatter ↔ C++ オブジェクトの正本定義。
// Discatier Core (Game / Mechanic / Aesthetic) と 1:1 対応する。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::crawler {

struct SourceRef {
    std::string url;
    std::optional<std::string> title;
    std::optional<std::string> fetched_at;   // ISO date (YYYY-MM-DD or full)
    std::optional<std::string> attribution;  // ライセンス表記 / 帰属
    std::optional<std::string> excerpt_policy;  // "summary-only" | "fair-use-quote" | "forbidden"
};

struct MechanicEntry {
    std::string name;
    std::optional<std::string> description;
    std::optional<std::string> intends;          // designer intent (Discatier Mechanic.intends)
    std::optional<std::string> intended_affect;  // Phase 4 affect
    // ---- 運営の想定感情 (Gap率分析用の構造化定義、任意) ----
    /** 運営が狙う感情極性 ("positive" | "negative" | "neutral")。 */
    std::optional<std::string> intended_valence;
    /** この mechanic が駆動するはずの aspect (lexicon の 8 種)。 */
    std::optional<std::vector<std::string>> intended_aspects;
    /** 運営が狙う Plutchik 感情 (任意)。 */
    std::optional<std::vector<std::string>> intended_emotions;
};

struct AestheticEntry {
    std::string name;
    std::optional<std::string> description;
};

struct GameKG {
    /** slug (kebab-case)。Game.id の dedup key として使う。 */
    std::string id;
    std::string title;
    /** 「大まかなジャンル」 */
    std::optional<std::string> genre;
    std::string workspace_id;
    std::vector<SourceRef> sources;
    std::vector<MechanicEntry> mechanics;
    std::vector<AestheticEntry> aesthetics;
    /** frontmatter の後ろにある人間用本文 (DB には入れない)。 */
    std::optional<std::string> body;
};

inline constexpr const char* DEFAULT_WORKSPACE = "knowledge";

// JSON (TS のオブジェクトと同じ形。undefined のキーは書かない)。
Json to_json(const SourceRef& v);
Json to_json(const MechanicEntry& v);
Json to_json(const AestheticEntry& v);
Json to_json(const GameKG& v);

} // namespace di::crawler
