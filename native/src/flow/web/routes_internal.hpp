#pragma once
// routes.cpp を Praesidium の静的解析 (1 単位 60 秒) に収まるよう分けた各単位が共有する状態・補助関数の宣言。
#include "flow/web/routes.hpp"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include "api/js_query.hpp"
#include "base/crypto.hpp"
#include "base/executor.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "flow/anatomia/index.hpp"
#include "flow/db/connection.hpp"
#include "flow/debatability.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/foresight/state.hpp"
#include "flow/games_md.hpp"
#include "flow/github_cli.hpp"
#include "flow/information_gate_runner.hpp"
#include "flow/mechanic_extract.hpp"
#include "flow/paper_blocks.hpp"
#include "flow/paper_markdown.hpp"
#include "flow/paper_reuse.hpp"
#include "flow/paper_review.hpp"
#include "flow/paper_revisions.hpp"
#include "flow/paper_voices.hpp"
#include "flow/persona_display.hpp"
#include "flow/project_premise.hpp"
#include "flow/spec_analyze.hpp"
#include "flow/web/page.hpp"

namespace di::flow::web {

namespace routes_detail {

JsonLimits body_limits(std::size_t max_bytes);

/** LLM の応答 (チェック系の JSON object) を読む上限。 */
inline const JsonLimits kLlmJsonLimits{1u << 20, 50'000, 32};



// ─── モジュールの状態 (TS の module-level Map / Set) ──────────────────────

/**
 * ペーパーレビュー待ち (sessionId → 草案 + 起動入力)。
 * /start で草案構築をバックグラウンド起動し、ready になったらブラウザが /paper で取得して
 * 直接編集 → /paper/approve で議論を開始する。フィールドは State::m で守る。
 */
struct WebPaperReview {
    FlowKind flow;
    std::optional<double> rounds;
    std::optional<double> turnsPerRound;
    std::optional<std::vector<std::string>> personaIds;
    /** 草案構築が完了したか (false の間はブラウザは待つ)。 */
    bool ready = false;
    /** 構築失敗時のメッセージ (あれば)。 */
    std::optional<std::string> error;
    std::optional<PaperDraft> draft;
    /** 集めた情報のサマリ (PaperReviewInfo の JSON。TS と同じく緩い object として持つ)。 */
    std::optional<Json> info;
    /** 無操作の自動開始タイマー (timeoutMs>0 時のみ。調整があるたび張り直す)。 */
    std::optional<Scheduler::Id> timer;
};


struct State {
    std::mutex m;
    std::shared_ptr<const FlowWebDeps> deps;
    /** flow=壁打ち の起動済みセッション (HTTP の後続発話を受けるため)。 */
    std::map<std::string, std::shared_ptr<SparringSession>> sparring;
    /** discussion/improvement の完了状態 (status の done 判定用)。 */
    std::set<std::string> finished;
    std::map<std::string, std::shared_ptr<WebPaperReview>> reviews;
    /** ペーパーを使い回したセッションの声の検索 (承認で議論に渡して外す)。 */
    std::map<std::string, VoiceLookup> voiceLookups;
    /** 進行中の議論の進行状態 (DELETE で止める)。 */
    std::map<std::string, std::shared_ptr<FlowRunState>> running;
    // 背景の仕事の数 (テストが終わりを待つため)
    std::condition_variable idle_cv;
    int inflight = 0;
};

State& state();

std::shared_ptr<const FlowWebDeps> current_deps();

bool submit_background(std::function<void()> fn);


inline const char* kBusyMessage = "サーバが混み合っています。しばらくしてから再度お試しください";

http::Response json_error(const std::string& message, int status);

http::Response not_initialized();

Json read_body(const http::Request& req, std::size_t max_bytes);

bool has_key(const Json& body, std::string_view key);

const Json* field(const Json& body, std::string_view key);

bool truthy_key(const Json& obj, std::string_view key);

std::string string_field(const Json& body, std::string_view key);

std::optional<std::string> raw_string(const Json& body, std::string_view key);

std::optional<std::string> model_or_null();

const std::vector<FlowTag>& valid_review_tags();

std::vector<FlowTag> sanitize_review_tags(const Json* raw, const std::vector<FlowTag>& fallback);

std::string strip_list_marker(const std::string& s);

std::optional<std::vector<std::string>> parse_issues_field(const Json* raw);

std::optional<std::vector<std::string>> parse_persona_ids(const Json* raw);

std::optional<double> read_optional_int(const Json* raw, double min, double max);


inline constexpr double kMaxSafeInteger = 9007199254740991.0;

std::optional<PaperFixedFields> fixed_seed_from_body(const Json& body);

std::optional<PaperFixedFields> merge_mechanics_context(const std::optional<PaperFixedFields>& seed, const std::vector<std::optional<std::string>>& texts);

std::string flow_theme_from_seed(const std::optional<PaperFixedFields>& seed, const std::string& legacy_theme);

PaperContent content_of(const PaperDraft& d);

const std::vector<std::string>& notion_source_fields();

bool needs_github_token(const std::string& url);

std::string resolve_additional_spec_text(const Json& body, const FlowWebDeps& d);

std::optional<std::vector<MechanicSummary>> resolve_anatomia_extra_mechanics(const std::string& theme, const std::string& project,
                                                                             const std::string& repo, const FlowWebDeps& d,
                                                                             const std::function<void(const std::string&)>& warn);


// ─── 合成の声 (学習フロー) ────────────────────────────────────────────────

struct Range {
    std::uint32_t first;
    std::uint32_t last;
};

inline constexpr Range kLetterNumber[] = {
#include "flow/unicode_letter_number.inc"
};

bool is_letter_or_number(std::uint32_t cp);

bool ascii_ci_equal_at(std::string_view s, std::size_t pos, std::string_view lower_kw);

std::size_t skip_js_spaces(std::string_view s, std::size_t p);

std::string normalize_synthetic_voice_text(const std::string& raw);

std::u16string to_utf16(std::string_view s);

std::string synthetic_similarity_key(const std::string& text);

std::set<std::uint64_t> synthetic_shingles(const std::u16string& key);

double synthetic_voice_similarity(const std::string& a, const std::string& b);

std::string compact_synthetic_label(const Json* raw, const std::string& fallback);

const Json* first_present(const Json* obj, std::initializer_list<const char*> keys);

std::string coerce_synthetic_polarity(const Json* raw);

std::string polarity_label(const std::string& p);


struct SyntheticCandidate {
    std::string content;
    std::string segment;
    std::string concern;
    std::string polarity;
    std::string similarityText;
};

std::optional<SyntheticCandidate> coerce_synthetic_candidate(const Json& raw);

std::vector<SyntheticCandidate> select_diverse_synthetic(const Json& raw_items, std::size_t limit = 8);

std::vector<LearningOpinion> synthesize_learning_opinions(const std::string& theme, const std::vector<GameMechanicEntry>& mechanics,
                                                          const std::string& mechanics_context, persona_engine::LLMClient& llm,
                                                          const std::function<void(const std::string&)>& warn);

Json default_paper_info(const std::optional<Json>& existing);

std::optional<Json> stored_paper_review_info(const std::string& session_id);

void save_paper_review_info(const std::string& session_id, const Json& info);


inline constexpr int REVIEW_INFO_SAMPLE_LIMIT = 9;

inline constexpr int DEBATABILITY_VOICE_LIMIT = 50;

Json review_info_samples(const std::vector<ContextVoice>& voices);

std::optional<Json> extract_json_object(const std::string& raw);

std::string short_string(const Json* raw, const std::string& fallback = "");

Json string_array(const Json* raw, std::size_t limit);

std::string paper_md_with_premise(const std::optional<std::string>& flow, const std::string& body_md);

Json suggest_fixes_for_paper(const std::optional<std::string>& flow, const PaperDraft& draft, const Json& info, persona_engine::LLMClient& llm,
                             const std::function<void(const std::string&)>& warn);

Json check_mechanics_knowledge(const PaperDraft& draft, const PaperFixedFields& fields, persona_engine::LLMClient& llm,
                               const std::function<void(const std::string&)>& warn);

Json voice_sim(bool possible, const char* confidence, const char* summary, const char* next_action);

Json assess_voice_simulation_readiness(const PaperDraft& draft, const Json& info, const std::optional<std::string>& flow);


/** 議論可能性チェックの LLM (config で無効でも手動チェックでは走らせる)。 */
struct ManualGate {
    std::shared_ptr<persona_engine::LLMClient> owned;
    persona_engine::LLMClient* llm = nullptr;
    int minArmableIssues = 2;
};

std::optional<ManualGate> manual_debatability_gate(const FlowKind& kind, const std::string& session_id, persona_engine::LLMClient& llm);

std::int64_t latest_revision_rev(const std::string& session_id);

Json paper_json(const PaperDraft& draft);

Json paper_payload_locked(const std::string& session_id, const PaperDraft& draft, const WebPaperReview* entry);

Json ok_with(const Json& payload, std::initializer_list<std::pair<const char*, Json>> before = {});

NewPaper new_paper_of(const std::string& session_id, const PaperDraft& d);


void schedule_web_auto_approve_locked(const std::string& session_id);

void commit_body_md_locked(const std::string& session_id, WebPaperReview& entry, const std::string& body_md, const std::string& change_summary,
                           const std::string& origin);

std::shared_ptr<WebPaperReview> rehydrate_draft_entry_locked(const std::string& session_id);

std::shared_ptr<WebPaperReview> find_entry_locked(const std::string& session_id, bool rehydrate);

void save_session_voices(const std::string& session_id, const std::string& theme, const std::optional<VoiceLookup>& lookup);

DispatchDeps base_dispatch_deps(const FlowWebDeps& d);

bool run_flow_in_background(const std::string& session_id, std::function<void(const std::shared_ptr<FlowRunState>&)> body,
                            const std::string& kind);

bool start_approved_flow(const std::string& session_id, const std::shared_ptr<WebPaperReview>& entry, const PaperDraft& final_paper,
                         const std::shared_ptr<const FlowWebDeps>& web_deps);

void schedule_web_auto_approve_locked(const std::string& session_id);

http::Response handle_start(http::Request& req);

http::Response handle_get_paper(http::Request& req);

http::Response not_ready(const char* message = "編集準備中です");

http::Response handle_form_apply(http::Request& req);

http::Response handle_debatability_check(http::Request& req);

http::Response handle_mechanics_check(http::Request& req);

http::Response handle_fix_apply(http::Request& req);

http::Response handle_block_review(http::Request& req);

http::Response handle_block_apply(http::Request& req);

std::string strip_heading_marker(const std::string& s);

http::Response handle_crawl(http::Request& req);

http::Response handle_revert(http::Request& req);

http::Response handle_approve(http::Request& req);

http::Response handle_say(http::Request& req);

double clamp_int_query(const std::optional<std::string>& v, double fallback, double min, double max);

http::Response handle_sessions(http::Request& req);

http::Response handle_delete(http::Request& req);

http::Response handle_status(http::Request& req);

} // namespace routes_detail
} // namespace di::flow::web
