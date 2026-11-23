#pragma once
// 論拠の応酬 1 論点ぶんの終わり方 (dialectic v2 P1)。run_ground_exchange が返し、まとめ文と決着表が同じ値を読む。
#include <string>

namespace di::flow::dialectic {

struct ExchangeEnd {
    /** 最後のターン番号 (この続きから進行役が決着を告げる)。 */
    int turn = 0;
    /** "settled" (応答待ちが無くなった) | "turn_cap" (応答ターンの上限で打ち切った) */
    std::string reason = "settled";
    /** 応答に使ったターン数。 */
    int answered = 0;
    /** 上限で打ち切った応答待ちの件数 (深さ上限で最初から打ち切った反論は含まない)。 */
    int cutPending = 0;
    /** 応答者の LLM 障害・空応答で応答できなかった件数 (崩れたにしない。未決に数える)。 */
    int errored = 0;
    /** 形の崩れた応答を深掘り (1 回の言い直し) した件数。 */
    int drilled = 0;
    /** そのうち言い直しで反論として成り立った件数。 */
    int drillRecovered = 0;
};

inline bool ended_by_turn_cap(const ExchangeEnd& e) { return e.reason == "turn_cap"; }

} // namespace di::flow::dialectic
