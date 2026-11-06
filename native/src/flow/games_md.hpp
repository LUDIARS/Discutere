#pragma once
// src/flow/games-md.ts の移植: data/games/<slug>.md のメカニクス frontmatter 読み書き。
// 学習フローが収集したメカニクスを、議論フローの調査 (investigate の load_theme_mechanics) が読める形
// (frontmatter の `mechanics:`) で永続する。読みは front_matter::parse、書きは front_matter::stringify。
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "flow/investigate.hpp"

namespace di::flow {

/** 1 メカニクス frontmatter エントリ (loadMechanics が読むキー)。 */
struct GameMechanicEntry {
    std::string name;
    std::optional<std::string> description;
    std::optional<std::string> intended_affect;
    std::optional<std::string> intended_valence;
    std::optional<std::vector<std::string>> intended_aspects;
    std::optional<std::vector<std::string>> intended_emotions;
    /** 出所ラベル (08)。learning 経由の書込は "crawl"。未指定は curated 扱いで読まれる。 */
    std::optional<MechanicSource> source;
};
/** 定義済みのキーだけを name, description, intended_*, source の順で書く。 */
Json to_json(const GameMechanicEntry& e);

struct UpsertGameMechanicsArgs {
    std::string gamesDir;
    std::string slug;
    std::string title;
    std::vector<GameMechanicEntry> mechanics;
};

/**
 * data/games/<slug>.md に mechanics を upsert する (name 一致でマージ)。
 * ファイルが無ければ最小 frontmatter (id/title/mechanics) で新規作成する。既存の他フィールドと本文は保持する。
 * slug が gamesDir の外を指す・既存ファイルが大きすぎる / 読めない front matter は std::runtime_error。
 * @returns 追加 + 更新されたメカニクス件数
 */
int upsert_game_mechanics(const UpsertGameMechanicsArgs& args);

/** MechanicSummary を frontmatter エントリへ変換する補助。 */
GameMechanicEntry mechanic_summary_to_entry(const MechanicSummary& m);

} // namespace di::flow
