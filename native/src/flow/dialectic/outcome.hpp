#pragma once
// src/flow/dialectic/outcome.ts の移植: 論点の結論の 3 区分 (止揚 / 論破 / 結論なし) — 決定的コード。
#include <optional>
#include <string>
#include <vector>

#include "flow/dialectic/store.hpp"

namespace di::flow::dialectic {

/** "aufheben" | "refuted" | "none" */
using IssueOutcome = std::string;

/** OUTCOME_LABEL[outcome] (止揚 / 論破 / 結論なし)。未知は空。 */
std::string outcome_label(const IssueOutcome& outcome);
IssueOutcome issue_outcome(const std::optional<std::string>& refutation, const std::vector<TensionRecord>& tensions);
/** 「結論なし」の理由 (Tension の状態から)。止揚には nullopt。 */
std::optional<std::string> no_conclusion_reason(const TensionRecord& t, const SynthesisRecord* synthesis = nullptr);
std::string render_outcome_line(const IssueOutcome& outcome, const std::string& detail);

} // namespace di::flow::dialectic
