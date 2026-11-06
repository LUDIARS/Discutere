#pragma once
// src/flow/paper-review.ts の移植: ディスカッションペーパー レビューゲート (議論開始前の人手レビュー & 調整) の共有コア。
//
// 議論/改善フローは情報ゲート (クロール) のあと、すぐにペルソナへペーパーを配って議論を始める。
// ここはその「直前」に人間が ペーパー (議題ブリーフ) と集めた情報を確認し、自然文で調整 → 承認してから
// 議論を始めるためのコア。トランスポート非依存 (Discord スレッド / Web UI の両方が使う):
//   - build_paper_draft  : investigate でメカニクス + 観点補足を組み、本文 md と情報サマリを付ける。
//   - apply_paper_edit   : 自然文の調整指示を LLM でペーパーに反映する (構造化編集 → 本文 md 再生成)。
//   - review_block       : 本文 md の 1 ブロックを LLM で改稿提案する (適用しない)。
//   - gather_evidence    : ブロックの根拠となる外部の声を集約し挿入用の段落を提案する。
//   - coerce_paper_draft : Web フォーム / 本文 md を PaperDraft に正規化する (本文 md 優先)。
//   - render_paper_review: ペーパー + 情報サマリを人間向け markdown にする。
//   - is_approval_text   : 「開始」「承認」等の承認語かどうか。
// 本文は markdown を正本とする (paper_markdown)。LLM の応答 JSON は llm_json_limits() の上限つきで読む。
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"
#include "flow/debatability.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/effect_predict.hpp"
#include "flow/investigate.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/tags.hpp"
#include "flow/voice_cache.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

/** 有効な観点タグ (編集で受理する集合)。 */
const std::vector<FlowTag>& valid_flow_tags();

/** 情報サマリのサンプル件数 / 件数カウントの上限。 */
inline constexpr int kPaperReviewSampleLimit = 9;
inline constexpr int kPaperReviewCountLimit = 50;

/** 人間レビュー対象のペーパー草案 (本文 bodyMd が正本、構造化フィールドは派生)。 */
struct PaperDraft : PaperContent {
    std::string bodyMd;
};
/** PaperContent のキー + bodyMd。 */
Json to_json(const PaperDraft& d);
PaperDraft paper_draft_from_json(const Json& j);

/** 構造化フィールドから本文 md を生成してドラフトにする。 */
PaperDraft with_derived_body(const PaperContent& content);
/** 本文 md から構造化フィールドを派生してドラフトにする (md を正本に保つ)。 */
PaperDraft with_derived_structure(const std::string& body_md, const PaperContent& fallback);

struct PaperSample {
    std::string content;
    std::string source;
};

struct PaperUnderstanding {
    bool ok = false;
    std::string rationale;
    std::vector<std::string> missingQuestions;
};
Json to_json(const PaperUnderstanding& u);

struct PaperFixSuggestion {
    std::string title;
    std::string reason;
    std::string suggestedChange;
    std::optional<double> appliedAt;
    std::optional<std::string> appliedText;
};
Json to_json(const PaperFixSuggestion& s);

struct PaperMechanicsKnowledge {
    bool ok = false;
    /** "low" | "medium" | "high" */
    std::string confidence;
    std::string summary;
    std::vector<std::string> knownMechanics;
    std::vector<std::string> missingQuestions;
};
Json to_json(const PaperMechanicsKnowledge& k);

struct PaperVoiceSimulation {
    bool possible = false;
    std::string confidence;
    std::string summary;
    std::string caveat;
    std::string nextAction;
};
Json to_json(const PaperVoiceSimulation& v);

/** ペーパーと併せて見せる「集めた情報」のサマリ。 */
struct PaperReviewInfo {
    /** テーマに紐づく外部の声の件数。kPaperReviewCountLimit で頭打ち。 */
    int voiceCount = 0;
    bool countCapped = false;
    std::vector<PaperSample> samples;
    std::optional<PaperUnderstanding> understanding;
    std::optional<DebatabilityResult> debatability;
    std::optional<std::vector<PaperFixSuggestion>> fixSuggestions;
    std::optional<PaperMechanicsKnowledge> mechanicsKnowledge;
    std::optional<PaperVoiceSimulation> voiceSimulation;
};
Json to_json(const PaperReviewInfo& info);

/** 評価の差し替え (テスト用)。既定は assess_debatability。 */
using AssessDebatabilityFn = std::function<DebatabilityResult(persona_engine::LLMClient&, const AssessDebatabilityArgs&)>;

struct PaperDebatabilityGate {
    /** 呼び出し側で with_cost_log 済みを渡す。 */
    persona_engine::LLMClient* llm = nullptr;
    int minArmableIssues = 2;
    AssessDebatabilityFn assess;
};

struct BuildPaperDraftDeps {
    std::optional<std::string> flow;
    YoutubeSearchFn youtubeSearch;
    std::optional<int> youtubeMaxComments;
    std::optional<std::string> gamesDir;
    std::optional<ListExternalVoicesFn> listExternalVoices;
    /** メカニクス LLM 増補に使う LLM (nullptr なら増補しない)。 */
    persona_engine::LLMClient* llm = nullptr;
    /** 基本理解の確認に使う LLM。nullptr なら llm、どちらも無ければ決定論フォールバック。 */
    persona_engine::LLMClient* understandingLlm = nullptr;
    /** メカニクスの目標件数 (既定 30)。 */
    std::optional<int> mechanicsTarget;
    /** 増補に使うモデル ("" / 未指定なら LLM 既定)。 */
    std::optional<std::string> enrichModel;
    /** 事前情報として先頭に差し込む追加メカニクス (名前で重複排除・先勝ち)。 */
    std::vector<MechanicSummary> extraMechanics;
    /** Web 固定フォーム由来の入力。指定時は本文 md を固定セクションで生成する。 */
    std::optional<PaperFixedFields> seed;
    /** 議論適性ゲート (09)。未指定 = ゲート無効。 */
    std::optional<PaperDebatabilityGate> debatability;
    WarnFn warn;
};

struct PaperDraftBuild {
    PaperDraft draft;
    PaperReviewInfo info;
};

/** メカニクスを名前で重複排除して連結する (先勝ち、名前は trim + 小文字で比べる)。 */
std::vector<MechanicSummary> merge_mechanics(const std::vector<MechanicSummary>& primary,
                                             const std::vector<MechanicSummary>& secondary);

struct AssessUnderstandingOpts {
    std::optional<std::string> model;
    WarnFn warn;
    std::optional<std::string> flow;
};

/** AI が基本的なゲーム内容を把握できているかの事前確認 (llm=nullptr は決定論)。LLM 失敗は決定論に倒す。 */
PaperUnderstanding assess_paper_understanding(const PaperFixedFields& fields, const std::vector<MechanicSummary>& mechanics,
                                              const std::vector<ContextVoice>& voices, persona_engine::LLMClient* llm,
                                              const AssessUnderstandingOpts& opts = {});

/** テーマを investigate してペーパー草案 + 集めた情報サマリを組み立てる (永続化しない)。 */
PaperDraftBuild build_paper_draft(const std::string& theme, const std::vector<FlowTag>& tags, const BuildPaperDraftDeps& deps);

struct PaperEditResult {
    PaperDraft draft;
    std::string changeSummary;
    bool applied = false;
};
Json to_json(const PaperEditResult& r);

struct ApplyPaperEditOpts {
    std::optional<std::string> model;
    WarnFn warn;
};

/**
 * 自然文の調整指示を LLM でペーパーに反映する (構造化編集)。
 * パース/検証に失敗したら元のドラフトを保ち applied=false を返す。
 */
PaperEditResult apply_paper_edit(const PaperDraft& draft, const std::string& instruction, persona_engine::LLMClient& llm,
                                 const ApplyPaperEditOpts& opts = {});

/** 外部入力 (Web フォームの直接編集 JSON / 本文 md) を PaperDraft に正規化する。不正は fallback。 */
PaperDraft coerce_paper_draft(const Json& raw, const PaperDraft& fallback);

/** ペーパー + 情報サマリを人間向け markdown にする (Discord 投稿 / Web 表示で共有)。 */
std::string render_paper_review(const PaperDraft& draft, const PaperReviewInfo& info);

/** 承認語かどうか (「開始」「承認」「approve」等)。 */
bool is_approval_text(std::string_view text);
/** 「戻す」語かどうか。 */
bool is_revert_text(std::string_view text);

struct BlockReviewResult {
    std::string blockId;
    std::string original;
    std::string proposed;
    std::string rationale;
    bool ok = false;
};
Json to_json(const BlockReviewResult& r);

struct ReviewBlockArgs {
    std::string bodyMd;
    std::string blockId;
    std::optional<std::string> instruction;
    std::optional<std::string> model;
    WarnFn warn;
};

/** 本文 md の 1 ブロックを LLM で改稿提案する (適用しない)。不在 / LLM 失敗は ok=false。 */
BlockReviewResult review_block(persona_engine::LLMClient& llm, const ReviewBlockArgs& args);

struct EvidenceResult {
    std::vector<PaperSample> voices;
    std::string suggestion;
};
Json to_json(const EvidenceResult& r);

struct GatherEvidenceArgs {
    std::string topic;
    ListExternalVoicesFn listExternalVoices;
    /** 提案段落を要約生成する LLM (nullptr なら箇条書き)。 */
    persona_engine::LLMClient* llm = nullptr;
    std::optional<std::string> model;
    std::optional<int> limit;
    WarnFn warn;
};

/** ブロックの根拠となる外部の声を集約し、挿入用の段落を提案する (KG へ書き込まない)。 */
EvidenceResult gather_evidence(const GatherEvidenceArgs& args);

} // namespace di::flow
