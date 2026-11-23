#pragma once
// 論述パターンのカタログ (spec/plan/2026-11-23-argument-patterns.md §3.1)。
//
// 正本は Histrio の script_patterns (版付き)。Histrio が動くまでは Di の data/argument-patterns.json を読む。
// ファイルが無い・読めないときは同じ内容の組み込み版を使う (テストで両者が一致することを確かめる)。
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"

namespace di::flow::dialectic {

/** 軸 A: 人生脚本パターン (反応の癖)。 */
struct ScriptPattern {
    std::string id;
    std::string label;
    /** 定立での癖。 */
    std::string position;
    /** 応酬での癖 (健全側)。 */
    std::string exchange;
    /** 押されたとき (自分の論拠が崩れた) の癖 (ストレス側)。 */
    std::string stress;
    /** OK牧場の揺れ先 (I+U+ / I+U- / I-U+ / I-U-)。記録用。 */
    std::string stressQuadrant;
};

/** 軸 B: 論じ方の好み (論拠の種類の偏り)。 */
struct ReasoningStyle {
    std::string id;
    std::string label;
    std::string line;
};

struct ArgumentPatternCatalog {
    std::string version;
    std::vector<ScriptPattern> scriptPatterns;
    std::vector<ReasoningStyle> reasoningStyles;
};

/** JSON からカタログを作る。形が違えば std::runtime_error。 */
ArgumentPatternCatalog parse_argument_pattern_catalog(const Json& j);
/** 組み込み版の JSON (data/argument-patterns.json と同じ内容)。 */
std::string_view embedded_argument_pattern_catalog_json();
/** プロセスで 1 回だけ読む。<repo_root>/data/argument-patterns.json → 組み込み版の順。 */
const ArgumentPatternCatalog& argument_pattern_catalog();

/** id か表示名で引く。無ければ nullptr。 */
const ScriptPattern* find_script_pattern(const ArgumentPatternCatalog& c, std::string_view id_or_label);
const ReasoningStyle* find_reasoning_style(const ArgumentPatternCatalog& c, std::string_view id_or_label);

} // namespace di::flow::dialectic
