#include "flow/dialectic/outcome.hpp"

namespace di::flow::dialectic {

std::string outcome_label(const IssueOutcome& outcome) {
    if (outcome == "aufheben") return "止揚";
    if (outcome == "refuted") return "論破";
    if (outcome == "none") return "結論なし";
    return "";
}

IssueOutcome issue_outcome(const std::optional<std::string>& refutation, const std::vector<TensionRecord>& tensions) {
    if (refutation && !refutation->empty()) return "refuted";
    for (const auto& t : tensions)
        if (t.status == "synthesized") return "aufheben";
    return "none";
}

std::optional<std::string> no_conclusion_reason(const TensionRecord& t, const SynthesisRecord* synthesis) {
    const std::string note = t.resolutionNote.value_or("(記録なし)");
    if (t.status == "synthesized") return std::nullopt;
    if (t.status == "compromised")
        return "折衷案は批准されたが止揚ではない" + (synthesis ? " — 折衷案: " + synthesis->text : std::string());
    if (t.status == "agreed_disagree")
        return "止揚案が両陣営に批准されず対立が残った" + (synthesis ? " — 最終候補: " + synthesis->text : std::string());
    if (t.status == "resolved") return "事実の対立は照会で確かめた: " + note;
    if (t.status == "unresolved_fact") return "未解決の事実問題: " + note;
    if (t.status == "agreed") return "(旧方式の合意判定) " + note;
    if (t.status == "aligned" || t.status == "partially_aligned") return "(旧方式のすり合わせ・詳細は記録なし)";
    if (t.status == "open") return "議論を打ち切った";
    return std::nullopt;
}

std::string render_outcome_line(const IssueOutcome& outcome, const std::string& detail) {
    return outcome_label(outcome) + ": " + detail;
}

} // namespace di::flow::dialectic
