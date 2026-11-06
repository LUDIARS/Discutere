#include "flow/dialectic/conclusion.hpp"

#include "base/strings.hpp"
#include "flow/dialectic/outcome.hpp"
#include "lib/sentiment_core/js_number.hpp"

namespace di::flow::dialectic {

namespace {

/** tension の決着 1 行を人間可読にする (止揚 / 結論なし + 理由)。 */
std::string settle_tension_line(const TensionRecord& t, const std::vector<SynthesisRecord>& syntheses) {
    const SynthesisRecord* s = nullptr;
    for (const auto& x : syntheses)
        if (x.tensionId == t.id) {
            s = &x;
            break;
        }
    if (t.status == "synthesized") {
        const std::string text = s ? s->text : std::string("(本文なし)");
        const std::string elevates = s && s->elevates && !s->elevates->empty() ? " — 新しい枠: " + *s->elevates : std::string();
        return render_outcome_line("aufheben", text + elevates);
    }
    return render_outcome_line("none", no_conclusion_reason(t, s).value_or("(記録なし)"));
}

std::string name_of(const std::map<std::string, std::string>& names, const std::string& id) {
    const auto it = names.find(id);
    return it != names.end() ? it->second : id;
}

} // namespace

RoundSummary settlement_to_round_summary(const IssueSettlement& s, const std::map<std::string, std::string>& persona_names,
                                         std::optional<double> winner_share) {
    std::vector<std::string> position_lines;
    for (const auto& p : s.positions) position_lines.push_back(name_of(persona_names, p.personaId) + " (" + p.stance + "): " + p.claim);
    const IssueOutcome outcome = issue_outcome(s.refutation, s.tensions);
    std::string tension_lines;
    if (s.refutation && !s.refutation->empty()) {
        tension_lines = render_outcome_line("refuted", *s.refutation);
    } else if (s.undecided && !s.undecided->empty()) {
        tension_lines = render_outcome_line("none", *s.undecided);
    } else if (!s.tensions.empty()) {
        std::vector<std::string> lines;
        for (const auto& t : s.tensions) lines.push_back(settle_tension_line(t, s.syntheses));
        tension_lines = str::join(lines, " / ");
    } else {
        tension_lines = render_outcome_line("none", "対立が成立しなかった");
    }
    const std::string share =
        winner_share ? " / 投票集中度 " + lib::sentiment_core::js_to_fixed(*winner_share, 2) : std::string();
    // 止揚だけを aufhebung に数える (折衷は summary に「折衷」として残る。dialectic.md §4)。
    std::vector<std::string> aufhebung;
    for (const auto& t : s.tensions) {
        if (t.status != "synthesized") continue;
        for (const auto& x : s.syntheses)
            if (x.tensionId == t.id) {
                if (!x.text.empty()) aufhebung.push_back(x.text);
                break;
            }
    }
    RoundSummary r;
    r.round = s.issue.ordinal;
    r.summary = "論点「" + s.issue.title + "」【" + outcome_label(outcome) + "】 — " + str::join(position_lines, " / ") + " ⇒ " +
                tension_lines + share;
    r.aufhebung = std::move(aufhebung);
    return r;
}

std::vector<std::string> collect_unresolved_facts(const std::vector<IssueSettlement>& settlements) {
    std::vector<std::string> notes;
    for (const auto& s : settlements)
        for (const auto& t : s.tensions)
            if (t.status == "unresolved_fact" && t.resolutionNote && !t.resolutionNote->empty()) notes.push_back(*t.resolutionNote);
    return notes;
}

ConclusionResult conclude_dialectic(persona_engine::LLMClient& llm, const ConcludeDialecticArgs& args) {
    std::vector<RoundSummary> rounds;
    for (std::size_t i = 0; i < args.settlements.size(); ++i) {
        const std::optional<double> share = i < args.voteResults.size() ? args.voteResults[i].winnerShare : std::nullopt;
        rounds.push_back(settlement_to_round_summary(args.settlements[i], args.personaNames, share));
    }

    const auto unresolved = collect_unresolved_facts(args.settlements);
    std::vector<std::string> annotation_parts;
    if (!unresolved.empty()) annotation_parts.push_back("未解決の事実問題: " + str::join(unresolved, " / "));
    if (args.debatabilityNote && !args.debatabilityNote->empty()) annotation_parts.push_back(*args.debatabilityNote);

    GenerateConclusionArgs ca;
    ca.theme = args.theme;
    ca.sessionId = args.sessionId;
    ca.paperId = args.paperId;
    ca.allUtterances = args.allUtterances;
    ca.allAufhebung = args.ratifiedAufheben;
    ca.rounds = std::move(rounds);
    ca.voteResults = args.voteResults;
    ca.warn = args.warn;
    ca.flow = args.flow;
    if (!annotation_parts.empty()) ca.annotation = str::join(annotation_parts, " / ");
    return generate_conclusion(llm, ca);
}

} // namespace di::flow::dialectic
