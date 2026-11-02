#pragma once
// Crawler md frontmatter <-> GameKG round-trip (src/crawler/md-format.ts の移植)。
//
// data/games/<slug>.md の構造:
//   ---
//   <YAML frontmatter — lib::front_matter で parse>
//   ---
//   <自由 markdown 本文 — DB には入れない>
//
// 必須キー: id, title, workspace_id
// 配列キー: sources, mechanics, aesthetics は空配列に正規化される
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "base/json.hpp"
#include "crawler/types.hpp"

namespace di::crawler {

class GameKGParseError : public std::runtime_error {
public:
    GameKGParseError(const std::string& message, std::optional<std::string> path = std::nullopt)
        : std::runtime_error(path ? message + " (file: " + *path + ")" : message), path_(std::move(path)) {}
    const std::optional<std::string>& path() const { return path_; }

private:
    std::optional<std::string> path_;
};

/** markdown を GameKG に読む。不正は GameKGParseError (front matter の YAML が読めない場合も)。 */
GameKG parse_game_kg(std::string_view markdown, const std::optional<std::string>& source_path = std::nullopt);

/** stringifyGameKG が matter.stringify に渡す frontmatter (undefined を除いたもの)。 */
Json game_kg_frontmatter(const GameKG& kg);

/** stringifyGameKG(kg) — matter.stringify(body, frontmatter)。実装は md_format_write.cpp。 */
std::string stringify_game_kg(const GameKG& kg);

} // namespace di::crawler
