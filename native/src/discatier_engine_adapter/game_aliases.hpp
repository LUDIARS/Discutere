#pragma once
// ゲーム名の別名 (略称⇄正式名・和名⇄英名) 展開 (src/discatier-engine-adapter/game-aliases.ts, #301)。
// 別名の出所: games タイトル "EN (JP)" の機械分解 + 機械導出できない口語略称の静的辞書。純関数。
// 文字列の長さは JS と同じく UTF-16 単位、小文字化は str::to_lower (JS toLowerCase の主要部分)。
#include <cstddef>
#include <string>
#include <vector>

namespace di::discatier_engine_adapter {

/** 機械導出できない口語略称グループ。 */
const std::vector<std::vector<std::string>>& static_alias_groups();

/** "EN (JP)" / "JP (英名)" タイトルを別名 (外側 + 括弧内) に分解する。 */
std::vector<std::string> parse_title_aliases(const std::string& title);

/** games タイトル群 + 静的略称 (+ extra) をマージした別名グループ集合 (2 要素以上のものだけ)。 */
std::vector<std::vector<std::string>> build_alias_groups(const std::vector<std::string>& titles,
                                                         const std::vector<std::vector<std::string>>& extra_groups = {});

/** 検索語を別名で拡張する (元の語は小文字で保持、最大 max 語)。 */
std::vector<std::string> expand_aliases(const std::vector<std::string>& terms,
                                        const std::vector<std::vector<std::string>>& groups, std::size_t max = 12);

} // namespace di::discatier_engine_adapter
