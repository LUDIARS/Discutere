#pragma once
// dialectic の結論の要約部 (dialectic v2 P3、spec/plan/2026-11-23-dialectic-v2-implementation-plan.md V4)。
//
// rounds エンジン用の converge プロンプト (投票の勝者 + 直近 20 発話を重視) は使わない。
// 入力は決着表 (settlement_table) だけ。LLM は要約部だけを書き、決着表はコードで本文に写す。
// 要約に失敗しても決着表だけで結論は成り立つ (concluded は決着の有無で決める)。
#include <optional>
#include <string>
#include <vector>

#include "flow/director.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::dialectic {

struct DialecticSummaryArgs {
    std::string theme;
    /** render_settlement_table の出力。 */
    std::string settlementTable;
    std::vector<std::string> unresolvedFacts;
    std::optional<std::string> model;
    WarnFn warn;
};

std::string build_dialectic_summary_prompt(const DialecticSummaryArgs& args);

/** JSON の summary (非空) を取り出す。無ければ nullopt。 */
std::optional<std::string> parse_dialectic_summary(const std::string& text);

/** 要約を 1 回生成する (JSON 崩れは同じ呼び出しで 1 回だけやり直す)。失敗は nullopt。 */
std::optional<std::string> generate_dialectic_summary(persona_engine::LLMClient& llm, const DialecticSummaryArgs& args);

/** 結論本文 = 要約 (無ければ定型の断り) + 決着表 + 未解決の事実問題。 */
std::string compose_dialectic_conclusion(const std::optional<std::string>& summary, const std::string& settlement_table,
                                         const std::vector<std::string>& unresolved_facts);

} // namespace di::flow::dialectic
