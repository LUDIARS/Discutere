#include "flow/dialectic/convergence.hpp"

#include "lib/sentiment_core/js_number.hpp"

namespace di::flow::dialectic {

ConvergenceVerdict assess_convergence(const ConvergenceInput& input) {
    ConvergenceVerdict v;
    v.signals.rebuttalsSettled = input.unresolvedRebuttals == 0;
    v.signals.voteConcentrated = input.winnerShare.has_value() && *input.winnerShare >= input.convergeShare;
    v.signals.claimsStale = input.newClaimsInWindow == 0;
    v.met = (v.signals.rebuttalsSettled ? 1 : 0) + (v.signals.voteConcentrated ? 1 : 0) + (v.signals.claimsStale ? 1 : 0);
    v.capReached = input.aufhebungTarget > 0 && input.ratifiedAufhebenCount >= input.aufhebungTarget;
    v.converged = v.capReached || v.met >= 2;

    const std::string share =
        input.winnerShare ? lib::sentiment_core::js_to_fixed(*input.winnerShare, 2) : std::string("n/a");
    const std::string parts = "未応答反論=" + js_number_to_string(input.unresolvedRebuttals) +
                              (v.signals.rebuttalsSettled ? " ✓" : "") + " / 投票集中度=" + share +
                              (v.signals.voteConcentrated ? " ✓" : "") + " / 新規claim(窓内)=" +
                              js_number_to_string(input.newClaimsInWindow) + (v.signals.claimsStale ? " ✓" : "");
    const std::string cap_note = v.capReached ? " / 批准済み止揚 " + js_number_to_string(input.ratifiedAufhebenCount) +
                                                    " 件が上限キャップ " + js_number_to_string(input.aufhebungTarget) + " に到達"
                                              : std::string();
    v.note = parts + " → " + std::to_string(v.met) + "/3" + cap_note + (v.converged ? " → 収束" : "");
    return v;
}

Json to_json(const ConvergenceVerdict& v) {
    Json signals = Json::object();
    signals["rebuttalsSettled"] = v.signals.rebuttalsSettled;
    signals["voteConcentrated"] = v.signals.voteConcentrated;
    signals["claimsStale"] = v.signals.claimsStale;
    Json o = Json::object();
    o["converged"] = v.converged;
    o["signals"] = std::move(signals);
    o["met"] = v.met;
    o["capReached"] = v.capReached;
    o["note"] = v.note;
    return o;
}

} // namespace di::flow::dialectic
