#pragma once
// 文字列の補助。内部表現は UTF-8 の std::string。
// TS の string は UTF-16 なので、長さ・切り出しを JS と揃えたいところでは js_* を使う。
// 正規表現 (std::regex) は外部入力に使わない (遅い・再帰で落ちる)。ここの線形走査の関数を使う。
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace di::str {

// ─── UTF-8 / UTF-16 ─────────────────────────────────────────────
/** 1 文字 (code point) を読む。不正なバイトは U+FFFD として 1 バイト進める。 */
std::uint32_t next_codepoint(std::string_view s, std::size_t& index);
void append_utf8(std::string& out, std::uint32_t codepoint);
std::vector<std::uint32_t> codepoints(std::string_view s);
std::string from_codepoints(const std::vector<std::uint32_t>& cps);
bool is_valid_utf8(std::string_view s);
/** 不正な UTF-8 を U+FFFD に置き換える。 */
std::string sanitize_utf8(std::string_view s);

/** JS の s.length (UTF-16 のコード単位数)。 */
std::size_t js_length(std::string_view s);
/** JS の s.slice(start, end) (UTF-16 単位、負数は末尾から)。end 省略は末尾まで。 */
std::string js_slice(std::string_view s, std::int64_t start, std::optional<std::int64_t> end = std::nullopt);
/** code point 単位の先頭 n 文字 ([...s].slice(0, n).join(""))。 */
std::string prefix_codepoints(std::string_view s, std::size_t n);
std::size_t count_codepoints(std::string_view s);

// ─── 空白 ────────────────────────────────────────────────────
/** JS の \s と同じ空白判定 (code point)。 */
bool is_js_space(std::uint32_t cp);
/** JS の String#trim。 */
std::string trim(std::string_view s);
std::string trim_start(std::string_view s);
std::string trim_end(std::string_view s);
/** s.replace(/\s+/g, " ") */
std::string collapse_whitespace(std::string_view s);
/** s.replace(/\s+/g, " ").trim() */
std::string squish(std::string_view s);

// ─── 大文字小文字 ────────────────────────────────────────────────
/** ASCII・Latin-1・全角英字の小文字化 (日本語は変えない)。JS の toLowerCase の主要部分。 */
std::string to_lower(std::string_view s);
std::string to_upper(std::string_view s);
std::string to_lower_ascii(std::string_view s);
bool iequals_ascii(std::string_view a, std::string_view b);

// ─── 分割・結合・置換 ─────────────────────────────────────────────
std::vector<std::string> split(std::string_view s, std::string_view separator);
/** 改行 (\r\n / \n / \r) で分ける。 */
std::vector<std::string> split_lines(std::string_view s);
std::string join(const std::vector<std::string>& parts, std::string_view separator);
std::string replace_all(std::string_view s, std::string_view from, std::string_view to);
bool contains(std::string_view s, std::string_view needle);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
std::string repeat(std::string_view s, std::size_t times);
/** JS の padStart (code point 単位で近似)。 */
std::string pad_start(std::string_view s, std::size_t width, char fill = ' ');

// ─── 数値 ────────────────────────────────────────────────────
std::optional<std::int64_t> parse_int(std::string_view s);
std::optional<double> parse_double(std::string_view s);
/** JS の Number(s) (前後の空白を許す。"" は 0。解釈できなければ NaN 相当で nullopt)。 */
std::optional<double> js_number(std::string_view s);

// ─── エスケープ ─────────────────────────────────────────────────
/** HTML のテキスト/属性に埋めるときのエスケープ (& < > " ')。 */
std::string escape_html(std::string_view s);
/** 正規表現・SQL LIKE 用ではなく、ログ表示用に制御文字を落とす。 */
std::string strip_control(std::string_view s);
/** SQL LIKE のメタ文字 (% _ \) を \ でエスケープする (ESCAPE '\' と併用)。 */
std::string escape_like(std::string_view s);

/** HTML からテキストだけを取り出す (線形時間)。script/style の中身は捨て、タグは空白にし、
 *  実体参照 (&amp; &lt; &gt; &quot; &#39; &#NNN; &#xHH;) を戻し、空白を 1 つに詰めて trim する。
 *  移植前 flow/spec-source.ts の stripHtml は正規表現の繰り返しで二乗時間だった (256KB で 13.8 秒)。 */
std::string strip_html(std::string_view html);
/** XML/HTML の実体参照を戻す (&amp; &lt; &gt; &quot; &apos; &#39; &#NNN; &#xHH;)。 */
std::string decode_entities(std::string_view s);

} // namespace di::str
