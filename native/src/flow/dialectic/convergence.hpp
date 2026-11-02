#pragma once
// src/flow/dialectic/convergence.ts の移植: 計測型収束 (respec 06 §2, dialectic.md §6) — コードのみで判定 (LLM なし)。
// (a) 未応答反論数 = 0 / (b) 投票の集中度 winnerShare >= convergeShare / (c) 直近 K ターン新規 claim なし
// の 2/3 成立で収束。aufhebungTarget は批准済み aufheben 数の上限キャップとしてのみ参照する。
#include <optional>
#include <string>

#include "base/json.hpp"

namespace di::flow::dialectic {

struct ConvergenceInput {
    double unresolvedRebuttals = 0;
    /** 直近 issue 決着投票の最多得票シェア (無投票/算出不能は nullopt)。 */
    std::optional<double> winnerShare;
    double convergeShare = 0;
    double newClaimsInWindow = 0;
    double ratifiedAufhebenCount = 0;
    double aufhebungTarget = 0;
};

struct ConvergenceSignals {
    bool rebuttalsSettled = false;
    bool voteConcentrated = false;
    bool claimsStale = false;
};

struct ConvergenceVerdict {
    bool converged = false;
    ConvergenceSignals signals;
    int met = 0;
    bool capReached = false;
    std::string note;
};

ConvergenceVerdict assess_convergence(const ConvergenceInput& input);
Json to_json(const ConvergenceVerdict& v);

} // namespace di::flow::dialectic
