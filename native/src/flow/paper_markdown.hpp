#pragma once
// src/flow/paper-markdown.ts の移植: ディスカッションペーパー本文の markdown 正本化 (ハイブリッド源泉モデル)。
// 構造化フィールド (theme / tags / supplement / mechanics) ⇄ 本文 md。DB / LLM に依存しない純関数のみ。
// paper_draft_to_markdown の出力は buildPaperSystem の旧経路の文面とバイト等価。
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"
#include "flow/investigate.hpp"
#include "flow/tags.hpp"

namespace di::flow {

/** ラウンドのまとめ (src/flow/discussion-paper.ts の RoundSummary)。 */
struct RoundSummary {
    int round = 0;
    std::string summary;
    std::vector<std::string> aufhebung;
};
Json to_json(const RoundSummary& r);

/** markdown 本文を持つペーパー (構造化フィールド + 本文 md)。 */
struct PaperContent {
    std::string theme;
    std::vector<FlowTag> tags;
    std::string supplement;
    std::vector<MechanicSummary> mechanics;
    /** 論点 (`# 論点` 節と round-trip)。 */
    std::optional<std::vector<std::string>> issues;
    std::optional<std::string> gameTitle;
    std::optional<std::string> discussionTheme;
    std::optional<std::string> discussionContent;
    std::optional<std::string> mechanicsContext;
    std::optional<std::string> themeSupplement;
};
/** TS の PaperContent と同じキー (固定欄は値があるときだけ)。 */
Json to_json(const PaperContent& c);
PaperContent paper_content_from_json(const Json& j);

inline constexpr std::string_view HEAD_HYPOTHESIS_MECHANICS = "# 仮説メカニクス (LLM抽出・未検証)";
inline constexpr std::string_view HEAD_ISSUES = "# 論点";

struct PaperFixedFields {
    std::string gameTitle;
    std::string discussionTheme;
    std::string discussionContent;
    std::string mechanicsContext;
    std::string themeSupplement;
};
Json to_json(const PaperFixedFields& f);

/** source=llm 以外 (curated/crawl/旧データ) のメカニクス。 */
std::vector<MechanicSummary> grounded_mechanics(const std::vector<MechanicSummary>& mechanics);
/** source=llm (LLM 増補 = 未検証の仮説) のメカニクス。 */
std::vector<MechanicSummary> hypothetical_mechanics(const std::vector<MechanicSummary>& mechanics);
/** grounded メカニクスを md 箇条書き (出所併記) にする (0 件はプレースホルダ)。 */
std::string grounded_mechanics_markdown(const std::vector<MechanicSummary>& mechanics);
/** 仮説メカニクス節 (見出し込み)。増補 0 件なら nullopt。 */
std::optional<std::string> hypothesis_section_markdown(const std::vector<MechanicSummary>& mechanics);
/** メカニクス本文 (grounded 箇条書き + 仮説節)。 */
std::string mechanics_body_markdown(const std::vector<MechanicSummary>& mechanics);
/** 論点節 (見出し込み)。0 件なら nullopt。 */
std::optional<std::string> issues_section_markdown(const std::optional<std::vector<std::string>>& issues);

/** 構造化ペーパー → 正本 markdown。 */
std::string paper_draft_to_markdown(const PaperContent& content);
/** 固定フォーム型の markdown から表示フィールドを取り出す。旧形式なら fallback で埋める。 */
PaperFixedFields paper_fixed_fields_from_markdown(std::string_view md, const PaperContent& fallback);
/** 正本 markdown → 構造化ペーパー (派生)。取れないフィールドは fallback で補う。tags は常に fallback。 */
PaperContent markdown_to_paper_draft(std::string_view md, const PaperContent& fallback);

/** base ブリーフから「議論の経過」以降 (進行ログ) を取り除く。 */
std::string strip_progress(std::string_view body_md);
/** base ブリーフ + 議論の経過 (ラウンドごとの まとめ / 止揚) + 結論 を 1 本の md に焼く (冪等)。 */
std::string render_progress_markdown(std::string_view base_body_md, const std::vector<RoundSummary>& rounds,
                                     const std::optional<std::string>& conclusion = std::nullopt);

} // namespace di::flow
