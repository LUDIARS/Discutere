#include "flow/dialectic/goal.hpp"

#include "base/strings.hpp"

namespace di::flow::dialectic {

std::vector<std::string> standing_ground_ids(const PositionRecord& p) {
    std::vector<std::string> out;
    for (const auto& g : p.grounds)
        if (g.state == "unchallenged" || g.state == "defended") out.push_back(g.id);
    return out;
}

namespace {
std::string scope(const PositionRecord& p) {
    return (p.qualifier && !p.qualifier->empty()) ? " (限定: " + *p.qualifier + ")" : "";
}
} // namespace

RefutationResult detect_refutation(const PositionRecord& a, const PositionRecord& b) {
    RefutationResult r;
    const bool a_down = standing_ground_ids(a).empty();
    const bool b_down = standing_ground_ids(b).empty();
    if (a_down) r.refuted.push_back(a);
    if (b_down) r.refuted.push_back(b);
    if (r.refuted.empty()) return r;
    if (r.refuted.size() == 2) {
        // どちらも支えられない = 今ある材料では決着できない。勝敗は付けない。
        r.undecided = "双方の論拠がすべて崩れたため勝敗は付けない。「" + a.claim + "」「" + b.claim + "」のどちらも、" +
                      "今ある材料では支えられない (決着には追加の材料が要る)";
        return r;
    }
    const PositionRecord& loser = a_down ? a : b;
    const PositionRecord& winner = a_down ? b : a;
    r.note = "「" + loser.claim + "」" + scope(loser) + "は根拠がすべて崩れて論破された。" + "「" + winner.claim + "」" +
             scope(winner) + "が根拠 " + str::join(standing_ground_ids(winner), ", ") + " で残る";
    return r;
}

} // namespace di::flow::dialectic
