#pragma once
// src/flow/persona-adopt-runner.ts の移植: C1 実在ユーザ採用ランナー (KG 集約 → 条件フィルタ → flow_persona upsert)。
// active KG の外部発話を話者アンカー (`ext:<source>:<authorId>`) で集約し、adopt_personas / evaluate_speakers に渡す。
// 個人データ方針: 集約は内部のフル精度で行い、保存ペルソナの露出名は `論者#xxxxxx` (adopt_personas 側でマスク)。
// 本モジュールは生 authorId を返さない。
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "flow/persona_adopt.hpp"

namespace di::flow {

struct AdoptRunOptions : AdoptOptions {
    /** source 種別フィルタ (例 "youtube")。未指定なら全 source。 */
    std::optional<std::string> sourceFilter;
    /** 採用判定のみ実施し、プールへ書き込まない。 */
    bool dry = false;
};

struct AdoptRunSummary {
    double speakers = 0;
    double utterances = 0;
    /** 採用 (upsert) 件数。dry のときは採用候補数。 */
    double adopted = 0;
    double rejected = 0;
    /** 却下理由 → 件数 (出現順)。 */
    std::vector<std::pair<std::string, double>> rejectedByReason;
};
/** TS と同じキー順 (speakers, utterances, adopted, rejected, rejectedByReason)。 */
Json to_json(const AdoptRunSummary& s);

/** KG から読む外部発話の上限 (DB の値も外部入力として課金する)。 */
inline constexpr sql::QueryLimits kAdoptKgReadLimits{2'000'000, 1024u << 20};

/** core DB と attribution を受けて話者集約する (テスト用に分けた本体)。 */
std::vector<SpeakerOpinions> collect_speaker_opinions(sql::Database& raw, crawler::sources::AttributionStore& attribution,
                                                      const std::string& workspace, const std::optional<std::string>& source_filter,
                                                      double* utterance_count = nullptr);

/** 集約済みの話者を採用判定し (dry なら書き込まない)、要約を返す。 */
AdoptRunSummary adopt_from_speakers(const std::vector<SpeakerOpinions>& speakers, double utterance_count, const AdoptRunOptions& opts);

/** active KG の外部発話を話者集約し、C1 採用条件で flow_persona へ upsert する。 */
AdoptRunSummary run_adopt_from_kg(const AdoptRunOptions& opts = {});

} // namespace di::flow
