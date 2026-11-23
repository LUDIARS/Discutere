#include "flow/dialectic/settlement_table.hpp"

#include <algorithm>

#include "base/strings.hpp"
#include "flow/dialectic/ground_state.hpp"
#include "flow/dialectic/outcome.hpp"

namespace di::flow::dialectic {

namespace {

std::string name_of(const std::map<std::string, std::string>& names, const std::string& id) {
    const auto it = names.find(id);
    return it != names.end() ? it->second : id;
}

const SynthesisRecord* synthesis_for(const TensionRecord& t, const std::vector<SynthesisRecord>& syntheses) {
    for (const auto& x : syntheses)
        if (x.tensionId == t.id) return &x;
    return nullptr;
}

std::string settle_tension_line(const TensionRecord& t, const std::vector<SynthesisRecord>& syntheses) {
    const SynthesisRecord* s = synthesis_for(t, syntheses);
    if (t.status == "synthesized") {
        const std::string text = s ? s->text : std::string("(本文なし)");
        const std::string elevates = s && s->elevates && !s->elevates->empty() ? " — 新しい枠: " + *s->elevates : std::string();
        return render_outcome_line("aufheben", text + elevates);
    }
    return render_outcome_line("none", no_conclusion_reason(t, s).value_or("(記録なし)"));
}

std::string side_label(const std::string& stance) { return stance == "con" ? "反対" : "賛成"; }

void push_grounds(std::vector<std::string>& lines, const std::string& heading, const std::vector<Ground>& grounds,
                  bool (*pred)(const GroundState&)) {
    std::vector<std::string> items;
    for (const auto& g : grounds)
        if (pred(g.state)) items.push_back("[" + g.id + "] " + str::js_slice(g.text, 0, 120));
    if (!items.empty()) lines.push_back("  - " + heading + ": " + str::join(items, " / "));
}

std::string render_exchange_end(const ExchangeEnd& e) {
    std::string out = ended_by_turn_cap(e) ? "応答ターンの上限で打ち切り (応答待ち " + std::to_string(e.cutPending) + " 件は未決)"
                                           : std::string("論拠のやり取りが落ち着いた");
    if (e.errored > 0) out += " / 応答できなかった " + std::to_string(e.errored) + " 件は未決";
    return out;
}

} // namespace

std::string render_settlement_outcome_line(const IssueSettlement& s) {
    if (s.refutation && !s.refutation->empty()) return render_outcome_line("refuted", *s.refutation);
    if (s.undecided && !s.undecided->empty()) return render_outcome_line("none", *s.undecided);
    if (!s.tensions.empty()) {
        std::vector<std::string> lines;
        for (const auto& t : s.tensions) lines.push_back(settle_tension_line(t, s.syntheses));
        return str::join(lines, " / ");
    }
    return render_outcome_line("none", "対立が成立しなかった");
}

std::string render_ratification_line(const IssueSettlement& s, const std::map<std::string, std::string>& persona_names) {
    std::vector<std::string> parts;
    for (const auto& syn : s.syntheses) {
        for (const auto& r : syn.ratification) {
            const auto pos = std::find_if(s.positions.begin(), s.positions.end(), [&](const PositionRecord& p) { return p.id == r.positionId; });
            const std::string who = pos != s.positions.end() ? side_label(pos->stance) + " (" + name_of(persona_names, pos->personaId) + ")"
                                                             : r.positionId;
            std::string part = who + " " + (r.verdict == "accept" ? "受け入れ" : "拒否");
            if (!r.preservedIds.empty()) part += " — 保存を確認: " + str::join(r.preservedIds, ", ");
            if (r.verdict != "accept" && !r.missing.empty()) part += " — 欠けている: " + str::join(r.missing, ", ");
            parts.push_back(part);
        }
        if (!syn.ratification.empty()) parts.push_back("修正 " + std::to_string(syn.revision) + " 回");
    }
    return str::join(parts, " / ");
}

std::string render_settlement_table(const SettlementTableArgs& args) {
    struct Row {
        int ordinal;
        std::string text;
    };
    std::vector<Row> rows;
    for (const auto& s : args.settlements) {
        std::vector<std::string> lines;
        const IssueOutcome outcome = issue_outcome(s.refutation, s.tensions);
        lines.push_back("## 論点 " + std::to_string(s.issue.ordinal) + "「" + s.issue.title + "」【" + outcome_label(outcome) + "】");
        lines.push_back("- 決着: " + render_settlement_outcome_line(s));
        for (const auto& p : s.positions) {
            lines.push_back("- " + side_label(p.stance) + " (" + name_of(args.personaNames, p.personaId) + "): " + p.claim +
                            (p.qualifier && !p.qualifier->empty() ? " (限定: " + *p.qualifier + ")" : "") + " — " +
                            render_ground_tally(tally_grounds(p.grounds)));
            push_grounds(lines, "残った根拠", p.grounds, is_standing_ground);
            push_grounds(lines, "未決の根拠 (前提に使わない)", p.grounds, is_unsettled_ground);
            push_grounds(lines, "崩れた根拠 (前提に使わない)", p.grounds, is_broken_ground);
        }
        if (s.exchangeEnd) lines.push_back("- 応酬: " + render_exchange_end(*s.exchangeEnd));
        const std::string ratification = render_ratification_line(s, args.personaNames);
        if (!ratification.empty()) lines.push_back("- 批准: " + ratification);
        rows.push_back({s.issue.ordinal, str::join(lines, "\n")});
    }
    for (const auto& issue : args.undiscussed)
        rows.push_back({issue.ordinal, "## 論点 " + std::to_string(issue.ordinal) + "「" + issue.title + "」【未討議】\n" +
                                           "- 決着: 議論しなかった (結論の前提に使わない)"});
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ordinal < b.ordinal; });
    std::vector<std::string> out;
    for (const auto& r : rows) out.push_back(r.text);
    return str::join(out, "\n\n");
}

} // namespace di::flow::dialectic
