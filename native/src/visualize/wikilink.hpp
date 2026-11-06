#pragma once
// Magic-link `[[<type>:<id>]]` の parse / format / 検証 (src/visualize/wikilink.ts の移植)。
// 形式が崩れた link (typo / 未知 prefix / 空 id) は失敗ではなく「無し」を返す (viewer が dangling link として警告できる)。
// 正規表現は使わず線形に走査する。
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.hpp"

namespace di::visualize {

inline constexpr std::array<std::string_view, 9> NODE_TYPES{"utt", "hyp", "gap", "mch", "aes", "aff", "ses", "game", "persona"};

struct Wikilink {
    std::string type;
    std::string id;
    bool operator==(const Wikilink&) const = default;
};
Json to_json(const Wikilink& link);

/** マジックリンク文字列を 1 件だけ parse (前後の空白は許す)。 */
std::optional<Wikilink> parse_wikilink(std::string_view text);
/** Wikilink → `[[type:id]]`。未知の type / 不正な id は std::invalid_argument。 */
std::string format_wikilink(const Wikilink& link);
/** 文字列内のすべての wikilink を抽出する。 */
std::vector<Wikilink> extract_wikilinks(std::string_view text);
/** "hypothesis" / "hyp" のどちらでも prefix に揃える。 */
std::optional<std::string> normalize_type(std::string_view input);
bool is_node_type(std::string_view value);

/** node type → Discatier table 名 / 出力 md ディレクトリ名。 */
extern const std::vector<std::pair<std::string, std::string>> TYPE_TO_TABLE;
extern const std::vector<std::pair<std::string, std::string>> TYPE_TO_DIR;
/** CLI 引数 / DESIGN.md の表記 (full word) → prefix。 */
extern const std::vector<std::pair<std::string, std::string>> FULL_TO_PREFIX;
/** 上の表を引く (無ければ nullopt)。 */
std::optional<std::string> lookup(const std::vector<std::pair<std::string, std::string>>& table, std::string_view key);

} // namespace di::visualize
