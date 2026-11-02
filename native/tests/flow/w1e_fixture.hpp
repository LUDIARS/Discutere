#pragma once
// w1-E の flow テストが共有する fixture の読み込み (native/tests/fixtures/flow/w1e_pure.json)。
#include "check.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "base/fsutil.hpp"
#include "base/json.hpp"

namespace w1e {

inline const di::Json& pure() {
    static const di::Json doc = [] {
        auto text = di::fs::read_file(di::test::fixtures_dir() + "/flow/w1e_pure.json", 64u << 20);
        if (!text) throw di::test::Failure("fixture missing: flow/w1e_pure.json");
        return di::parse_json_or_throw(*text, {64u << 20, 5'000'000, 64});
    }();
    return doc;
}

inline std::string s(const di::Json& j) { return di::json_stringify(j); }

inline std::optional<std::string> opt(const di::Json& o, const char* key) {
    if (!o.contains(key) || o[key].is_null()) return std::nullopt;
    return o[key].get<std::string>();
}

/** テストの lcg (tests/flow/turn-scheduler.test.ts と同じ)。 */
inline std::function<double()> lcg(std::uint32_t seed_init) {
    auto seed = std::make_shared<std::uint32_t>(seed_init);
    return [seed] {
        *seed = static_cast<std::uint32_t>(static_cast<std::uint64_t>(*seed) * 1664525u + 1013904223u);
        return static_cast<double>(*seed) / 4294967296.0;
    };
}

} // namespace w1e
