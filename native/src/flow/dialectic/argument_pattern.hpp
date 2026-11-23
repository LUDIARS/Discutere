#pragma once
// 論述パターンの割り当てと prompt の行 (spec/plan/2026-11-23-argument-patterns.md §3.2〜3.3)。
//
// - 割り当て: 議論者ごとに軸 A (人生脚本パターン) と軸 B (論じ方の好み) を 1 つずつ。seed から決定的に決め、
//   同じ陣営に同じパターンを並べない。ペーパーの「# 論じ方の癖」節で人が上書きできる。代理議論の人物と進行役には付けない。
// - prompt: 定立は「定立の癖」、応酬は「応酬の癖」。その論点で自分の論拠が崩れていたら (押されている) ストレス側へ
//   コードで切り替える。LLM の呼び出しは増えない。癖の名前は名乗らせない。
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "flow/dialectic/argument_pattern_catalog.hpp"
#include "flow/personas.hpp"

namespace di::flow::dialectic {

struct ArgumentPatternOverride {
    std::optional<std::string> scriptPattern;
    std::optional<std::string> reasoningStyle;
};

/**
 * ペーパー本文の「# 論じ方の癖」節を読む (キーは人物名)。行の形は「- 名前: パターン / 論じ方」(どちらかだけも可)。
 * パターン・論じ方は id か表示名。カタログに無い値は捨てる。
 */
std::map<std::string, ArgumentPatternOverride> parse_argument_pattern_overrides(const std::string& paper_md,
                                                                                 const ArgumentPatternCatalog& catalog);

/**
 * cast の議論者 (進行役・代理以外) に割り当てる (scriptPattern / reasoningStyle を id で入れる)。
 * 上書きを先に置き、残りは seed と陣営から決まる位置から、その陣営で未使用のものを順に取る。
 */
void assign_argument_patterns(std::vector<FlowPersona>& cast, const std::string& seed,
                              const std::map<std::string, ArgumentPatternOverride>& overrides, const ArgumentPatternCatalog& catalog);

enum class ArgumentPhase { position, exchange };

/** prompt の「# あなた」に足す行 (割り当てが無ければ空)。pressed は応酬で自分の論拠が崩れているとき。 */
std::vector<std::string> argument_pattern_lines(const FlowPersona& persona, ArgumentPhase phase, bool pressed,
                                                const ArgumentPatternCatalog& catalog);

} // namespace di::flow::dialectic
