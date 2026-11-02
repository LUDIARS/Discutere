#pragma once
// src/flow/persona-adopt.ts の移植: 実在ユーザ採用 (C1)。
//
// クロール済みの外部発話を話者アンカー (`ext:<source>:<authorId>`) で集約し、「意思ある人 (ゲーム嗜好あり)」を
// ペルソナとしてプールに採用する。採用条件 (spec/feature/flow/persona-pool.md §C1):
//  - 意見が全ゲーム横断で minOpinions (既定 10) 以上。
//  - 全ネガ / 全ポジを除外。
//  - ゲーム間で affect の差分 (gap) がある (≥2 ゲーム + per-game 連結ベクトルが同一でない)。
// affect 解像度向上 (#125, 非 embedding): 短文はゲーム単位で連結してからベクトル化し、per-game ベクトルを
// エンゲージメント重み付け平均する。口調は付けない。露出名は `論者#xxxxxx`。保存は source_speaker_id で upsert。
#include <optional>
#include <string>
#include <vector>

#include "flow/persona_pool.hpp"

namespace di::flow {

struct SpeakerOpinion {
    std::string text;
    std::optional<std::string> gameSlug;
    /** opinion-score (いいね数 + 1)。未指定は 1。 */
    std::optional<double> weight;
};

/** 1 話者の意見群 (KG 集約結果)。 */
struct SpeakerOpinions {
    std::string speakerId;
    std::optional<std::string> source;
    std::vector<SpeakerOpinion> opinions;
};

struct AdoptOptions {
    std::optional<double> minOpinions;  // 既定 10
    std::optional<double> polarityEps;  // 既定 0.05
    std::optional<double> gameGapEps;   // 既定 0.05
};

/** "too-few" | "all-positive" | "all-negative" | "no-game-gap" */
using RejectReason = std::string;

struct AdoptRejection {
    std::string speakerId;
    RejectReason reason;
    double count = 0;
};

struct AdoptCandidate {
    std::string speakerId;
    std::optional<std::string> source;
    std::vector<double> affect;
    double count = 0;
    double polarityBias = 0;
    double affectDispersion = 0;
};

struct EvaluateSpeakersResult {
    std::vector<AdoptCandidate> candidates;
    std::vector<AdoptRejection> rejected;
};

struct AdoptResult {
    std::vector<PoolPersona> adopted;
    std::vector<AdoptRejection> rejected;
};

Json to_json(const AdoptCandidate& c);
Json to_json(const AdoptRejection& r);

/** 採用判定 (純関数)。プール書込はしない。 */
EvaluateSpeakersResult evaluate_speakers(const std::vector<SpeakerOpinions>& speakers, const AdoptOptions& opts = {});

/** 話者意見群を評価し、採用候補をプールへ upsert する (C1-a)。既存 adopted の typicality も再計算する。 */
AdoptResult adopt_personas(const std::vector<SpeakerOpinions>& speakers, const AdoptOptions& opts = {});

} // namespace di::flow
