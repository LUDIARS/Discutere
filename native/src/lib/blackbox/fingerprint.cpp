#include "lib/blackbox/fingerprint.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "base/strings.hpp"

namespace di::lib::blackbox {

namespace {

constexpr int kMaxDepth = 256;

/** JS の配列添字形式のキー (正準な十進表記で 2^32-2 以下) なら値を返す。 */
std::optional<std::uint64_t> array_index(const std::string& key) {
    if (key.empty() || key.size() > 10) return std::nullopt;
    if (key.size() > 1 && key[0] == '0') return std::nullopt;
    std::uint64_t v = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (v > 4294967294ULL) return std::nullopt;
    return v;
}

/** UTF-16 のコード単位列 (JS の文字列比較の単位)。 */
std::vector<std::uint16_t> utf16_units(const std::string& s) {
    std::vector<std::uint16_t> out;
    std::size_t i = 0;
    while (i < s.size()) {
        std::uint32_t cp = str::next_codepoint(s, i);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<std::uint16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<std::uint16_t>(cp));
        }
    }
    return out;
}

Json sort_value(const Json& value, int depth) {
    if (depth > kMaxDepth) throw std::runtime_error("canonical_json: nested too deep");
    if (value.is_array()) {
        Json out = Json::array();
        for (const auto& v : value) out.push_back(sort_value(v, depth + 1));
        return out;
    }
    if (value.is_object()) {
        // Object.keys(src).sort() → 新しい object に入れ直す (添字キーは数値順で先頭に来る)。
        struct Key {
            std::string name;
            std::optional<std::uint64_t> index;
            std::vector<std::uint16_t> units;
        };
        std::vector<Key> keys;
        for (auto it = value.begin(); it != value.end(); ++it) keys.push_back({it.key(), array_index(it.key()), utf16_units(it.key())});
        std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
            if (a.index && b.index) return *a.index < *b.index;
            if (a.index != b.index) return a.index.has_value();
            return a.units < b.units;
        });
        Json out = Json::object();
        for (const auto& k : keys) out[k.name] = sort_value(value.at(k.name), depth + 1);
        return out;
    }
    return value;
}

} // namespace

std::string canonical_json(const Json& value) { return json_stringify(sort_value(value, 0)); }

std::string rule_fingerprint(const Json& when, const Json& output) {
    return canonical_json(Json{{"when", when}, {"output", output}});
}

bool same_output(const Json& a, const Json& b) { return canonical_json(a) == canonical_json(b); }

} // namespace di::lib::blackbox
