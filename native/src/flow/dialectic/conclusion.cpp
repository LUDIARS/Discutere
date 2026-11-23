#include "flow/dialectic/conclusion.hpp"

#include "base/strings.hpp"
#include "flow/cost_logger.hpp"
#include "flow/dialectic/conclusion_summary.hpp"
#include "flow/dialectic/outcome.hpp"
#include "flow/dialectic/settlement_table.hpp"
#include "lib/sentiment_core/js_number.hpp"

namespace di::flow::dialectic {

namespace {

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
    const std::string tension_lines = render_settlement_outcome_line(s);
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
    SettlementTableArgs ta;
    ta.settlements = args.settlements;
    ta.undiscussed = args.undiscussed;
    ta.personaNames = args.personaNames;
    const std::string table = render_settlement_table(ta);
    const auto unresolved = collect_unresolved_facts(args.settlements);

    std::optional<std::string> summary;
    if (!args.settlements.empty()) {
        CostLogContext ctx;
        ctx.flow = args.flow;
        ctx.sessionId = args.sessionId;
        ctx.location = "summary";
        auto logged = with_cost_log(llm, std::move(ctx));
        DialecticSummaryArgs sa;
        sa.theme = args.theme;
        sa.settlementTable = table;
        sa.unresolvedFacts = unresolved;
        sa.model = args.model;
        sa.warn = args.warn;
        summary = generate_dialectic_summary(logged, sa);
    }

    const bool concluded = !args.settlements.empty();
    std::string body = concluded ? std::string(CONVERGE_PREFIX) + compose_dialectic_conclusion(summary, table, unresolved)
                                 : std::string("(結論なし)");
    if (args.debatabilityNote && !args.debatabilityNote->empty()) body += "\n\n※ " + *args.debatabilityNote;

    PersistConclusionArgs pa;
    pa.theme = args.theme;
    pa.sessionId = args.sessionId;
    pa.paperId = args.paperId;
    pa.flow = args.flow;
    pa.summary = body;
    pa.concluded = concluded;
    pa.aufhebung = args.ratifiedAufheben;
    pa.utteranceCount = static_cast<std::int64_t>(args.allUtterances.size());
    pa.engineVersion = args.engineVersion;
    pa.warn = args.warn;
    persist_flow_conclusion(pa);
    return {body, concluded};
}

} // namespace di::flow::dialectic
