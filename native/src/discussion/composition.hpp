#pragma once
// 議論パーティ編成 (src/discussion/composition.ts, pure)。
// - 総数 = max(minTotal, ceil(想定発話数 / 4) + 1)
// - 内訳: 司会 ×1 + キーマン ×keymanCount + 意見 ×(残り)
// - 司会 / キーマン = 固定モデル、意見 = weighted 抽選
// - 賛否はペルソナに固定せず、ターン時に decide_stance() で振る
// RNG は注入式 ([0,1) を返す)。
#include <functional>
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::discussion {

/** "facilitator" | "keyman" | "opinion" */
using Tier = std::string;
/** "pro" | "con" | "neutral" */
using Stance = std::string;
using Rng = std::function<double()>;

struct WeightedModel {
    std::string model;
    /** 相対重み (正の数)。 */
    double weight = 0;
};

struct PartyConfig {
    double expectedUtterances = 20;
    int keymanCount = 2;
    std::string facilitatorModel;
    std::string keymanModel;
    std::vector<WeightedModel> opinionModels;
    int minTotal = 4;
};

struct PartyMember {
    /** スロット id (パーティ内で安定)。例 "keyman-1" */
    std::string slotId;
    Tier tier;
    std::string model;
    std::string personaName;
    std::string speechStyle;
    std::vector<std::string> traits;
    bool operator==(const PartyMember&) const = default;
};

Json to_json(const PartyMember& m);

/** 総数を算出する。 */
int party_size(double expected_utterances, int min_total = 4);
/** weighted 抽選でモデルを 1 つ選ぶ。 */
std::string pick_weighted_model(const std::vector<WeightedModel>& models, const Rng& rng);
/** パーティを編成する。 */
std::vector<PartyMember> form_party(const PartyConfig& cfg, const Rng& rng);
/** 続行時の半数入替 (司会は不変、キーマンはモデル維持で別人格、意見はモデルも再抽選)。 */
std::vector<PartyMember> swap_half(const std::vector<PartyMember>& party, const PartyConfig& cfg, const Rng& rng);
/** ターン時の賛否決定。司会は neutral、キーマン/意見は pro/con を 50:50。 */
Stance decide_stance(const PartyMember& member, const Rng& rng);

} // namespace di::discussion
