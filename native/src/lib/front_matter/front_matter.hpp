#pragma once
// gray-matter (4.0.3) の置き換え。Markdown 先頭の YAML front matter を読み書きする。
//
// 読み (parse): YAML の部分集合だけを読む。範囲を超える YAML は拒否する (std::nullopt + 理由)。
//   読める: ブロックの mapping / sequence、フロー [a, b] / {k: v} (1 行)、平文・'...'・"..." のスカラー、
//          | / > のブロックスカラー (chomping + / - と字下げ指示)、# コメント。
//   型の解決は js-yaml 3 の safeLoad と同じ: null (~ null Null NULL 空), bool (true/True/TRUE/false/...),
//          int (10 進・0x・0o 相当の 0 始まり 8 進・0b、_ 区切り), float (1.5, .5, 1e3, .inf, .nan),
//          timestamp (YYYY-MM-DD 等) は JS の Date を JSON にしたときと同じ ISO 文字列にする。
//   拒否: アンカー (&)・エイリアス (*)・タグ (!)・複合キー (?)・ディレクティブ (%)・文書区切り・重複キー・
//          タブの字下げ・60 進数 (1:20)。
//   上限: front matter 1MiB、入れ子 32、要素 100,000 (lorica::input_budget)。
// 書き (stringify): matter.stringify(content, data) と同じ出力 (js-yaml 3.15 の safeDump 書式)。
#include <optional>
#include <string>
#include <string_view>

#include "base/json.hpp"

namespace di::lib::front_matter {

struct Matter {
    /** front matter の値 (無ければ空 object)。 */
    Json data = Json::object();
    /** front matter を除いた本文。 */
    std::string content;
    /** "---" で始まる front matter があったか。 */
    bool has_matter = false;
};

struct Limits {
    std::size_t max_bytes = 1u << 20;
    std::size_t max_depth = 32;
    std::size_t max_elements = 100'000;
};

/** matter(text)。front matter が無い文字列は data={} / content=text。読めない YAML は nullopt。 */
std::optional<Matter> parse(std::string_view text, std::string* error = nullptr, const Limits& limits = {});

/** YAML の部分集合だけを読む (front matter の中身)。 */
std::optional<Json> parse_yaml(std::string_view yaml, std::string* error = nullptr, const Limits& limits = {});

/** matter.stringify(content, data)。data が空 object なら content だけ (末尾改行を補う)。
 *  content を parse し直すので、content の先頭の front matter が読めなければ std::runtime_error を投げる。 */
std::string stringify(std::string_view content, const Json& data);

/** js-yaml 3.15 safeDump(data) (末尾改行つき)。 */
std::string dump_yaml(const Json& data);

} // namespace di::lib::front_matter
