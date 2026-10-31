#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace di::url {

struct Url {
    std::string scheme;    // "http" / "https" / "ws" / "wss" (小文字)
    std::string host;      // IPv6 は角括弧なし
    std::uint16_t port = 0;  // 省略時は scheme の既定
    std::string target;    // パス + クエリ ("/" から始まる)
    std::string path;      // クエリを除いたパス
    std::string query;     // "?" を除いたクエリ
    std::string fragment;
    bool explicit_port = false;
    /** "https://host[:port]" (new URL(u).origin)。 */
    std::string origin() const;
    /** host が IPv6 なら角括弧つき。 */
    std::string host_for_header() const;
};

/** 絶対 URL を読む (http/https/ws/wss のみ)。userinfo つき・不正な形は nullopt。 */
std::optional<Url> parse(std::string_view text);

/** encodeURIComponent。 */
std::string encode_component(std::string_view s);
/** decodeURIComponent (不正な % は nullopt)。 */
std::optional<std::string> decode_component(std::string_view s);
/** application/x-www-form-urlencoded の値を戻す (+ → 空白)。不正な % はそのまま。 */
std::string decode_form(std::string_view s);
/** クエリ文字列 ("a=1&b=2") を読む。同名は出現順。 */
std::vector<std::pair<std::string, std::string>> parse_query(std::string_view query);
/** URLSearchParams#toString 相当。 */
std::string build_query(const std::vector<std::pair<std::string, std::string>>& params);

} // namespace di::url
