#pragma once
// src/flow/foresight/camps.ts の移植: 未来予測フェーズの話者割当 (pure)。
//
// 本議論の発話から話者を引き継ぎ、改革派 (賛同) / 保守派 (否定) に振り分ける。
// エンジン (rounds / dialectic) に依らず FlowUtteranceRecord だけから決めるので、
// dispatch の後段でどちらのエンジンの結果にも同じ規則で使える。
//
//   - 陣営は本議論の賛否 (pro / con) とは独立に、初登場順に交互に割り当てる
//   - 各話者には本議論での自分の発言 (直近 OWN_STATEMENT_LIMIT 件) を持たせ、視点と口調の一貫性に使う
//   - 進行役 (facilitator) は陣営に入れず、提案と予測を担う
#include <optional>
#include <string>
#include <vector>

#include "flow/director.hpp"
#include "flow/foresight/prompts.hpp"

namespace di::flow::foresight {

struct ForesightRole {
    std::string personaId;
    std::string personaName;
    std::string role;
};

struct ForesightSpeaker {
    std::string personaId;
    std::string personaName;
    std::string role;
    ForesightCamp camp;
    /** 本議論での自分の発言 (発言順、各 OWN_STATEMENT_CHARS 字まで)。 */
    std::vector<std::string> ownStatements;
};

struct ForesightCast {
    ForesightRole facilitator;
    std::vector<ForesightSpeaker> speakers;
};

Json to_json(const ForesightSpeaker& s);
Json to_json(const ForesightCast& c);

/** 話者に持たせる本議論の発言の件数と 1 件の字数 (コードポイント)。 */
inline constexpr std::size_t OWN_STATEMENT_LIMIT = 2;
inline constexpr std::size_t OWN_STATEMENT_CHARS = 240;
/** 発言回数の上限 (C++ 版の追加の暴走ガード。config は 1..20 に丸めている)。 */
inline constexpr int MAX_CAMP_TURNS = 1000;

/**
 * 本議論の発話から未来予測フェーズのキャストを作る。
 * 進行役以外の話者が 2 人未満 (改革派・保守派のどちらかが 0 人) なら nullopt (フェーズを行わない)。
 */
std::optional<ForesightCast> build_foresight_cast(const std::vector<FlowUtteranceRecord>& utterances);

/** 発言回数を往復単位 (偶数、最低 1 往復) に揃える。turns=1 でも保守派が必ず話す。 */
int normalize_camp_turns(double turns);

/** i 番目のターンの段階 (往復ごとに 成立条件 → 反証 → 確かめ方、4 往復目以降は確かめ方のまま)。 */
CampStage camp_stage_at(int turn_index);

/**
 * 議論ターンの話者順。改革派と保守派を交互に並べ、各陣営の中は巡回する。
 * 改革派から始める (提案を擁護する側が先に口火を切る)。回数は normalize_camp_turns で往復単位に揃える。
 */
std::vector<ForesightSpeaker> build_camp_turn_order(const std::vector<ForesightSpeaker>& speakers, double turns);

} // namespace di::flow::foresight
