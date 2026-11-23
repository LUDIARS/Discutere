#pragma once
// 論点ごとの決着表 (dialectic v2 P3・P7、spec/plan/2026-11-23-dialectic-v2-implementation-plan.md V4)。
//
// 結論本文の正本。LLM に書かせずコードで組み立てる:
//   - 全予定論点を 1 行ずつ (議論しなかった論点は「未討議」)
//   - 区分 (止揚 / 論破 / 結論なし) と理由
//   - 陣営ごとの主張と根拠の内訳 (残った / 未決 / 崩れた)、根拠 id 付き
//   - 応酬の終わり方 (落ち着いた / 上限で打ち切り)
//   - 止揚の批准 (陣営ごとの accept/reject と保存を確認した根拠)
// 結論の要約 (LLM) はこの表を入力にし、表に無い前提を使わせない。
#include <map>
#include <string>
#include <vector>

#include "flow/dialectic/conclusion.hpp"
#include "flow/dialectic/store.hpp"

namespace di::flow::dialectic {

/** 論点の区分の 1 行 (止揚 / 論破 / 結論なし + 理由)。ペーパー経過と決着表が共用する。 */
std::string render_settlement_outcome_line(const IssueSettlement& s);

/** 止揚の批准結果の 1 行 (批准の無い論点は空)。 */
std::string render_ratification_line(const IssueSettlement& s, const std::map<std::string, std::string>& persona_names);

struct SettlementTableArgs {
    std::vector<IssueSettlement> settlements;
    /** 議論しなかった論点 (収束での打ち切り・停止)。 */
    std::vector<IssueRecord> undiscussed;
    std::map<std::string, std::string> personaNames;
};

/** 決着表の markdown (`## 論点 N「…」【区分】` の繰り返し)。論点が 1 件も無ければ空。 */
std::string render_settlement_table(const SettlementTableArgs& args);

} // namespace di::flow::dialectic
