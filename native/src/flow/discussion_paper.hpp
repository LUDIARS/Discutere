#pragma once
// src/flow/discussion-paper.ts の移植: ディスカッションペーパー生成 (OVERVIEW §5, discussion.md step 3) と永続化。
// ロール × LLM 性質で配布内容が変わる:
//   - debater (クラウド LLM): 共通項目のみ / opinion: + ユーザ意見 / isLocal: ロール問わず + ユーザ意見
//   - 機密タグ: ユーザ意見の代わりに (synthetic) ラベル付き想定意見
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "flow/investigate.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/personas.hpp"
#include "flow/tags.hpp"

namespace di::flow {

struct ContextVoice {
    std::string content;
    std::string source;
    std::optional<std::string> sourceUrl;
    bool operator==(const ContextVoice&) const = default;
};
Json to_json(const ContextVoice& v);
ContextVoice context_voice_from_json(const Json& j);

struct DiscussionPaper {
    std::string paperId;
    std::string sessionId;
    std::string theme;
    std::vector<FlowTag> tags;
    std::vector<MechanicSummary> mechanics;
    std::string supplement;
    std::vector<RoundSummary> rounds;
    /** 議論ブリーフ本文の正本 markdown。指定時は build_paper_system がこれをそのまま載せる。 */
    std::optional<std::string> bodyMd;
};

/** Omit<DiscussionPaper, "paperId" | "rounds"> (永続化の入力)。 */
struct NewPaper {
    std::string sessionId;
    std::string theme;
    std::vector<FlowTag> tags;
    std::vector<MechanicSummary> mechanics;
    std::string supplement;
    std::optional<std::string> bodyMd;
};

/** ペルソナへの配布内容 (ターンごとに組み立て)。 */
struct PersonaPaper {
    std::string theme;
    std::string supplement;
    std::string mechanicsText;
    std::string previousRoundsText;
    std::vector<std::string> currentRoundUtterances;
    std::optional<std::string> userOpinionsText;
    std::optional<std::string> bodyMd;
};

struct NamedUtterance {
    std::string personaName;
    std::string text;
};

struct BuildPersonaPaperArgs {
    DiscussionPaper paper;
    FlowPersona persona;
    FlowStance stance;
    std::vector<NamedUtterance> currentRoundUtterances;
    std::vector<ContextVoice> userVoices;
    std::vector<std::string> syntheticOpinions;
};

/** 機密タグ時の想定意見データを生成する (mechanics.intended_affect から簡易合成)。 */
std::vector<std::string> synthesize_opinions(const std::vector<MechanicSummary>& mechanics);
/** ペルソナ用ディスカッションペーパーを組み立てる (ターンごとに呼ぶ)。 */
PersonaPaper build_persona_paper(const BuildPersonaPaperArgs& args);
/** セッション不変の安定部 (議題 + 観点補足 + メカニクス)。bodyMd があればそれをそのまま返す。 */
std::string build_paper_system(const PersonaPaper& p);
/** スタンス → 立場指示行 (未知のスタンスは空文字)。 */
std::string stance_line(const FlowStance& stance);
/** ペルソナ固有 + 可変部 (前ラウンド結果 / 当ラウンド意見 / ユーザの声)。 */
std::string build_persona_user_prompt(const PersonaPaper& p, const FlowStance& stance, const FlowPersona& persona);
/** build_persona_user_prompt + build_paper_system の合成 (単一文字列・後方互換)。 */
std::string paper_to_prompt(const PersonaPaper& p, const FlowStance& stance, const FlowPersona& persona);

// ── 永続化 ──────────────────────────────────────────────────────────────────

/** "paper" | "conclusion" */
using DiscussionTitleSource = std::string;
/** "ai" | "chat" | "discord" | "unknown" */
using DiscussionOriginUi = std::string;
/** "all" | "ai" | "chat" */
using FlowSessionScope = std::string;

struct TitleCacheMeta {
    std::optional<std::string> discussionType;
    std::optional<DiscussionOriginUi> originUi;
};

std::string normalize_discussion_display_title(std::string_view raw, std::string_view fallback = "(無題)");
void upsert_discussion_title_cache(const std::string& session_id, std::string_view raw_title,
                                   const DiscussionTitleSource& source = "paper",
                                   std::optional<std::int64_t> updated_at = std::nullopt, const TitleCacheMeta& meta = {});

/** discussion_paper を保存し paperId を返す (同 session の行があれば status='started' に upsert)。 */
std::string persist_paper(const NewPaper& paper, const std::string& flow = "discussion",
                          const std::optional<DiscussionOriginUi>& origin_ui = std::nullopt);

/** 編集ゲートのドラフト行 (status='draft') の復元用スナップショット。 */
struct DraftPaperRow {
    std::string paperId;
    std::string flow;
    std::string theme;
    std::vector<FlowTag> tags;
    std::vector<MechanicSummary> mechanics;
    std::string supplement;
    std::string bodyMd;
};
struct PaperSnapshotRow : DraftPaperRow {
    std::string status;
};

/** 編集中ドラフトを status='draft' で永続 (既存行は内容だけ更新し status を変えない)。 */
std::string persist_draft_paper(const NewPaper& paper, const std::string& flow = "discussion",
                                const std::optional<DiscussionOriginUi>& origin_ui = std::nullopt);
std::optional<DraftPaperRow> get_draft_paper(const std::string& session_id);
std::optional<PaperSnapshotRow> get_paper_snapshot(const std::string& session_id);
bool has_flow_conclusion(const std::string& session_id);
void update_paper_body(const std::string& paper_id, const std::string& body_md);
void update_paper_derived(const std::string& paper_id, const std::string& supplement,
                          const std::vector<MechanicSummary>& mechanics);
/** 議論適性ゲートの結果を debatability_json に記録する (nullopt で消去)。対象行が無ければ no-op。 */
void set_paper_debatability(const std::string& session_id, const std::optional<Json>& result);
/** ペーパー編集画面の LLM チェック情報を保存する (debatability_json も同期)。 */
void set_paper_review_info(const std::string& session_id, const std::optional<Json>& info);
/** LLM チェック情報を復元する (旧データは debatability_json から最小表示情報を組み立てる)。 */
std::optional<Json> get_paper_review_info(const std::string& session_id);
std::optional<std::string> get_paper_body_by_session(const std::string& session_id);
std::optional<std::int64_t> get_discussion_number_by_session(const std::string& session_id);

/** "all" | "draft" | "live" | "concluded" */
using FlowSessionState = std::string;

struct FlowSessionRow {
    std::optional<std::int64_t> discussionNo;
    std::string sessionId;
    std::string title;
    std::string theme;
    std::string flow;
    std::string status;
    std::string discussionType;
    std::string originUi;
    std::int64_t createdAt = 0;
    std::int64_t updatedAt = 0;
    std::int64_t utterances = 0;
    std::optional<std::int64_t> concluded;
};
/** SELECT の列順 (better-sqlite3 の行オブジェクトと同じ)。 */
Json to_json(const FlowSessionRow& r);

struct ListFlowSessionsResult {
    std::vector<FlowSessionRow> rows;
    std::int64_t total = 0;
};
struct ListFlowSessionsOptions {
    FlowSessionState state = "all";
    FlowSessionScope scope = "all";
    std::int64_t limit = 100;
    std::int64_t offset = 0;
};
/** 議論一覧を state 絞り込み + ページングで返す (新しい順)。未知の state / scope は "all"。 */
ListFlowSessionsResult list_flow_sessions(const ListFlowSessionsOptions& opts);
/** 1 議論 (session) の全永続データを 1 トランザクションで削除する。対象が無ければ false。 */
bool delete_flow_session(const std::string& session_id);

} // namespace di::flow
