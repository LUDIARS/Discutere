#pragma once
// 論拠 (定立の根拠) の状態の語彙と判定 (dialectic v2, spec/plan/2026-11-23-dialectic-v2-implementation-plan.md V1)。
//
// 状態は 5 つ:
//   unchallenged  反論されなかった               — 残った
//   defended      反論に答えて守った             — 残った
//   unsettled     打ち切り・応答障害で決着せず   — 残ったにも崩れたにも数えない (v2 で追加)
//   challenged    反論が成立して崩れた           — 崩れた
//   conceded      本人が認めた                   — 崩れた
// 文字列のまま持つ (DB の列・旧データと互換)。比較はこのヘッダの関数に集める。
#include <string>
#include <string_view>
#include <vector>

#include "flow/dialectic/store.hpp"

namespace di::flow::dialectic {

inline constexpr std::string_view GROUND_UNCHALLENGED = "unchallenged";
inline constexpr std::string_view GROUND_DEFENDED = "defended";
inline constexpr std::string_view GROUND_UNSETTLED = "unsettled";
inline constexpr std::string_view GROUND_CHALLENGED = "challenged";
inline constexpr std::string_view GROUND_CONCEDED = "conceded";

/** 崩れずに残った (反論なし / 守った)。 */
inline bool is_standing_ground(const GroundState& s) { return s == GROUND_UNCHALLENGED || s == GROUND_DEFENDED; }
/** 崩れた (反論が成立 / 本人が認めた)。 */
inline bool is_broken_ground(const GroundState& s) { return s == GROUND_CHALLENGED || s == GROUND_CONCEDED; }
/** 決着しなかった (打ち切り・応答障害)。 */
inline bool is_unsettled_ground(const GroundState& s) { return s == GROUND_UNSETTLED; }

/** 露出・決着表に出す日本語ラベル (未知の値はそのまま返す)。 */
std::string ground_state_label(const GroundState& s);

/** 陣営 1 つぶんの内訳。 */
struct GroundTally {
    int unchallenged = 0;
    int defended = 0;
    int unsettled = 0;
    int broken = 0;
    int total = 0;
    int standing() const { return unchallenged + defended; }
};

GroundTally tally_grounds(const std::vector<Ground>& grounds);

/** 「反論なし a / 守った b / 未決 c / 崩れた d」(0 件の項目も出す。比較しやすくするため)。 */
std::string render_ground_tally(const GroundTally& t);

/** 状態で絞った根拠 id (挿入順)。 */
std::vector<std::string> ground_ids_where(const std::vector<Ground>& grounds, bool (*pred)(const GroundState&));

} // namespace di::flow::dialectic
