#pragma once
// src/visualize/conclusions.ts の移植: 結論リスト + 論述データ閲覧 (#66)。
// 収束した議論 (gap status = closed/converged) の【収束】まとめを「結論」として一覧化し、
// 選んだ結論の裏にある論述データ (議論ログ / 止揚ストック / 高評価意見) を辿れるようにする。
// 話者は露出用ラベルにマスクする (§6: persona は表示名、ext は 論者#xxxx、それ以外は「人間」)。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "core/index.hpp"
#include "crawler/sources/attribution_store.hpp"

namespace di::visualize {

/** 結論の出自。"gap" = 旧 auto-discussion (design_gaps)、"flow" = 新フロー (flow_conclusion)。 */
using ConclusionKind = std::string;

struct ConclusionSummary {
    /** 結論の一意キー。旧フローは design_gap の id、新フローは "flow:<sessionId>"。 */
    std::string gapId;
    ConclusionKind kind;
    std::string sessionId;
    std::string title;
    /** 【収束】まとめ本文 (プレフィックス除去済)。無ければ nullopt (JSON では null)。 */
    std::optional<std::string> conclusion;
    std::int64_t utteranceCount = 0;
    std::int64_t aufhebungCount = 0;
    std::int64_t updatedAt = 0;
    /** 話題のサイズ — 議論ボリューム (発話数)。キャッシュ経由のときだけ。 */
    std::optional<std::int64_t> discussionVolume;
    /** 話題のサイズ — 外部クロール材料件数。-1 = 未算出、nullopt = キャッシュ非経由。 */
    std::optional<std::int64_t> materialCount;
};

struct ConclusionMechanic {
    std::string name;
    std::string description;
    std::optional<std::string> intendedAffect;
};

/** ディスカッションペーパー (議論開始時にペルソナへ配布した「議題ブリーフ」)。新フロー結論のみ。 */
struct ConclusionPaper {
    std::string theme;
    std::vector<std::string> tags;
    std::string supplement;
    std::vector<ConclusionMechanic> mechanics;
};

struct ConclusionOpinion {
    std::string speaker;
    std::string content;
    std::int64_t score = 0;
    std::optional<std::string> source;
    std::optional<std::string> sourceUrl;
};

struct ConclusionLine {
    std::string speaker;
    std::string content;
    std::optional<std::string> source;
    std::optional<std::string> sourceUrl;
};

struct ConclusionDetail : ConclusionSummary {
    /** 議論ペーパー (議題ブリーフ)。新フロー結論のみ。旧 design_gap 議論は nullopt (JSON では null)。 */
    std::optional<ConclusionPaper> paper;
    std::vector<std::string> aufhebungen;
    std::vector<ConclusionOpinion> topOpinions;
    std::vector<ConclusionLine> transcript;
};

/** TS のオブジェクトと同じキー順 (discussionVolume / materialCount / intendedAffect は値があるときだけ)。 */
Json to_json(const ConclusionSummary& s);
Json to_json(const ConclusionPaper& p);
Json to_json(const ConclusionDetail& d);
/** JSON から戻す (形が違うフィールドは既定値)。golden・キャッシュの読み戻し用。 */
ConclusionSummary conclusion_summary_from_json(const Json& j);
ConclusionDetail conclusion_detail_from_json(const Json& j);

/** 収束済み議論の結論を新しい順に一覧する。 */
std::vector<ConclusionSummary> list_conclusions(core::Core& core, const std::string& workspace_id, std::int64_t limit = 20);

/**
 * 1 件の結論の裏にある論述データを取り出す。gap が無ければ nullopt。
 * attribution を渡すと発話ごとに出所 (source / sourceUrl) を付与する (§6 露出制御)。
 */
std::optional<ConclusionDetail> get_conclusion_detail(core::Core& core, const std::string& workspace_id, const std::string& gap_id,
                                                      crawler::sources::AttributionStore* attribution = nullptr);

/** 露出用の話者ラベル (§6 個人マスク)。テスト用に公開。 */
std::string conclusion_speaker_label(const std::optional<std::string>& speaker_id);

} // namespace di::visualize
