#pragma once
// llm_gateway_tests.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各パートが共有する準備。
// @ludiars/llm-gateway の移植 (lib/lapilli/packages/llm-gateway/src/index.test.ts) +
// golden (native/tests/golden/w1-B-llm_gateway.gen.ts が記録した Node の出力)。
#include "check.hpp"

#include <cmath>

#include "base/fsutil.hpp"
#include "lib/llm_gateway/llm_gateway.hpp"

using namespace di;
using namespace di::lib::llm_gateway;

namespace llm_gateway_support {
Json load(const std::string& rel) {
    auto text = fs::read_file(test::fixtures_dir() + rel);
    if (!text) throw std::runtime_error("fixture missing: " + rel);
    return parse_json_or_throw(*text, {8u << 20, 1'000'000, 64});
}
std::vector<HistoryTurn> make_turns(int n) {
    std::vector<HistoryTurn> t;
    for (int i = 0; i < n; ++i) t.push_back({"user", "turn " + std::to_string(i) + " " + std::string(40, 'x')});
    return t;
}
} // namespace llm_gateway_support
