#pragma once
// src/flow/dialectic/goal.ts の移植: 論破の判定 [4] (dialectic.md §4.5) — コードのみ。
// unchallenged / defended の根拠は残り、challenged / conceded は崩れた。根拠がすべて崩れたら論破。
// v2 P2: unsettled (打ち切り・応答障害) は残ったにも崩れたにも数えない。残った根拠が無く未決が残る陣営は
// 論破にせず、論点は結論なし (未決) にする。
#include <optional>
#include <string>
#include <vector>

#include "flow/dialectic/store.hpp"

namespace di::flow::dialectic {

/** 残った根拠 id (1 つも無ければその主張は論破された)。 */
std::vector<std::string> standing_ground_ids(const PositionRecord& p);

struct RefutationResult {
    std::vector<PositionRecord> refuted;
    std::optional<std::string> note;
    std::optional<std::string> undecided;
};

RefutationResult detect_refutation(const PositionRecord& a, const PositionRecord& b);

} // namespace di::flow::dialectic
