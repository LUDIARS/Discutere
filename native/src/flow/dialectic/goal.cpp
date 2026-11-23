#include "flow/dialectic/goal.hpp"

#include "base/strings.hpp"
#include "flow/dialectic/ground_state.hpp"

namespace di::flow::dialectic {

std::vector<std::string> standing_ground_ids(const PositionRecord& p) { return ground_ids_where(p.grounds, is_standing_ground); }

namespace {
std::string scope(const PositionRecord& p) {
    return (p.qualifier && !p.qualifier->empty()) ? " (限定: " + *p.qualifier + ")" : "";
}

/** 残った根拠が無い陣営の状態。 */
enum class Down { No, AllBroken, OnlyUnsettled };

Down down_state(const PositionRecord& p) {
    if (!standing_ground_ids(p).empty()) return Down::No;
    return ground_ids_where(p.grounds, is_unsettled_ground).empty() ? Down::AllBroken : Down::OnlyUnsettled;
}
} // namespace

RefutationResult detect_refutation(const PositionRecord& a, const PositionRecord& b) {
    RefutationResult r;
    const Down da = down_state(a);
    const Down db = down_state(b);
    if (da == Down::No && db == Down::No) return r;
    if (da == Down::AllBroken && db == Down::AllBroken) {
        r.refuted = {a, b};
        // どちらも支えられない = 今ある材料では決着できない。勝敗は付けない。
        r.undecided = "双方の論拠がすべて崩れたため勝敗は付けない。「" + a.claim + "」「" + b.claim + "」のどちらも、" +
                      "今ある材料では支えられない (決着には追加の材料が要る)";
        return r;
    }
    if (da == Down::OnlyUnsettled || db == Down::OnlyUnsettled) {
        // 崩れずに残った根拠が無く、決着しなかった根拠だけがある陣営がいる = 未決。論破にしない (v2 P2)。
        const PositionRecord& unsettled = da == Down::OnlyUnsettled ? a : b;
        r.undecided = "「" + unsettled.claim + "」の根拠は崩れずに残ったものが無いが、決着しなかった根拠 (" +
                      str::join(ground_ids_where(unsettled.grounds, is_unsettled_ground), ", ") +
                      ") が残るため、勝敗は付けない (応酬の打ち切り・応答できなかった分は未決として扱う)";
        return r;
    }
    const PositionRecord& loser = da == Down::AllBroken ? a : b;
    const PositionRecord& winner = da == Down::AllBroken ? b : a;
    r.refuted.push_back(loser);
    r.note = "「" + loser.claim + "」" + scope(loser) + "は根拠がすべて崩れて論破された。" + "「" + winner.claim + "」" +
             scope(winner) + "が根拠 " + str::join(standing_ground_ids(winner), ", ") + " で残る";
    return r;
}

} // namespace di::flow::dialectic
