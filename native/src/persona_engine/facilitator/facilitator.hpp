#pragma once
// Facilitator — 議論の活性化と収束を司る per-discussion オーケストレータ (src/persona-engine/facilitator/facilitator.ts)。
// spec/feature/facilitator/DESIGN.md。
//
// 各 discussion-of-gap session を独自 tick で見張り:
//  - 発言間隔が空いた (idle) → 新しい視点の persona を 1 体生成して投入し議論を広げる
//  - 参加 persona が maxPersonas を超えた → 議論を要約して収束、 gap を closed に
// gap が closed になるまで存在する (= 収束したら当該 session の管理を終える)。
//
// 投稿は contextProvider.post_utterance を通すので、既存の Discord relay に乗る。
// 定期実行は global_scheduler() で起こし、仕事 (DB + LLM) は background_executor() で走らせる (periodic.hpp)。
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/index.hpp"
#include "persona_engine/context_provider.hpp"
#include "persona_engine/db/personas_repo.hpp"
#include "persona_engine/facilitator/periodic.hpp"
#include "persona_engine/facilitator/prompts.hpp"
#include "persona_engine/llm/client.hpp"
#include "persona_engine/llm/tier_routing.hpp"
#include "persona_engine/types.hpp"
#include "runtime_settings/store.hpp"

namespace di::persona_engine::facilitator {

struct FacilitatorOptions {
    /** 見張り周期 ms (既定 30_000) */
    std::optional<double> tickMs;
    /** 発言が無くなってから「停滞」とみなす空白 ms (既定 120_000) */
    std::optional<double> idleGapMs;
    /** 参加 persona がこの数を超えたら強制収束 (安全上限、既定 20) */
    std::optional<double> maxPersonas;
    /** 止揚 (アウフヘーベン) がこの数たまったら収束 (既定 3) */
    std::optional<double> aufhebungTarget;
    /** LLM model 上書き (tierModels 未設定時の既定、または cheap/strong の最終 fallback) */
    std::optional<std::string> model;
    /** tier 別モデル (任意)。収束/止揚/視点追加を strong に振り分ける。 */
    std::optional<TierModels> tierModels;
};

/** 議論収束イベント (gap closed の直後に発火)。discord 依存は持たせない純データ。 */
struct FacilitatorConvergedEvent {
    std::string gapId;
    std::string sessionId;
    /** 議論 session の scene ("discord:<guild>/<channel>" or "gap:<gapId>")。無ければ nullopt。 */
    std::optional<std::string> scene;
    std::string title;
    /** 収束まとめ本文 (【収束】プレフィックス無し)。 */
    std::string summary;
};

struct FacilitatorDeps {
    std::shared_ptr<core::Core> core;
    LLMClientPtr llm;
    std::shared_ptr<DiscussionContextProvider> contextProvider;
    std::shared_ptr<db::PersonasRepo> personas;
    std::string workspaceId;
    /** 省略時は console_logger()。 */
    std::shared_ptr<Logger> logger;
    FacilitatorOptions options;
    /** 収束トリガーのライブ設定取得 (任意)。tick ごとに読む (Web UI の変更を再起動なしで反映)。 */
    std::function<runtime_settings::FacilitatorTuning()> tuning;
    /** 収束して gap を closed にした直後に呼ばれる (任意)。例外は握って warn (収束処理は止めない)。 */
    std::function<void(const FacilitatorConvergedEvent&)> onConverged;
};

struct SessionState {
    std::int64_t lastUtteranceCount = 0;
    bool armed = true;  // 次の idle で介入してよいか (発言があると再 arm)
    bool operator==(const SessionState&) const = default;
};

struct SessionMetrics {
    std::int64_t utteranceCount = 0;
    std::int64_t distinctPersonas = 0;
    std::int64_t lastActivityAt = 0;
    std::int64_t now = 0;
};

/** "active" | "wait" | "intervene" */
using FacilitatorMode = std::string;

struct EvaluateResult {
    FacilitatorMode mode;
    SessionState state;
};

/**
 * 介入タイミング判定 (純粋関数)。
 * - 新しい発言があった → active (再 arm)
 * - 発言間隔が空いた (idle) かつ armed → intervene (止揚判定 + 拡張/収束は呼び出し側)
 */
EvaluateResult evaluate(const SessionMetrics& metrics, const SessionState& state, double idle_gap_ms);

inline constexpr const char* FACILITATOR_PERSONA_ID = "facilitator";

/** 進行役 persona のプロフィール (収束のまとめ発言者 / headless seed の開幕も担う)。 */
const PersonaSeed& FACILITATOR_PERSONA();

struct ConvergeNowResult {
    bool ok = false;
    std::optional<std::string> error;
};

class Facilitator {
public:
    /** 表の確保 (reaction / aufhebung_stock / exclusions / directives) と進行役 persona の登録まで行う。 */
    explicit Facilitator(FacilitatorDeps deps);
    ~Facilitator();
    Facilitator(const Facilitator&) = delete;
    Facilitator& operator=(const Facilitator&) = delete;

    void start();
    void stop();
    /** 1 周分を手動実行 (テスト / デバッグ)。 */
    void tick_once();
    /**
     * 指定議論を即時に強制収束する (管制 UI の「まとめて閉じる」)。auto-tick 未起動でも動く。
     * 既に closed/converged/dismissed の gap は何もしない。
     */
    ConvergeNowResult converge_now(const std::string& session_id, const std::string& gap_id);

private:
    struct Discussion {
        std::string sessionId;
        std::string gapId;
    };
    runtime_settings::FacilitatorTuning get_tuning() const;
    std::vector<Discussion> list_active_discussions();
    SessionMetrics metrics_for(const std::string& session_id, std::int64_t now);
    std::vector<RecentUtterance> recent_utterances(const std::string& session_id, int limit);
    std::string speaker_label(const std::optional<std::string>& speaker_id);
    std::string gap_topic(const std::string& gap_id);
    std::vector<std::string> existing_persona_names(const std::string& session_id);
    std::vector<std::string> list_aufhebung(const std::string& gap_id);
    void stock_aufhebung(const std::string& gap_id, const std::string& summary);
    std::vector<std::string> top_opinions(const std::string& session_id, std::size_t limit);
    std::size_t judge_and_stock_aufhebung(const Discussion& d);
    void expand(const Discussion& d);
    void converge(const Discussion& d);
    std::string gap_title(const std::string& gap_id);
    std::optional<std::string> end_converged_sessions(const std::string& session_id);
    void close_gap(const std::string& gap_id);
    void intervene(const Discussion& d, const SessionMetrics& metrics);
    std::optional<std::string> model_for(const std::string& kind) const;
    sql::Database& raw() { return deps_.core->raw(); }

    FacilitatorDeps deps_;
    runtime_settings::FacilitatorTuning default_tuning_;
    std::mutex states_mutex_;
    std::map<std::string, SessionState> states_;
    PeriodicTask timer_;
};

std::shared_ptr<Facilitator> create_facilitator(FacilitatorDeps deps);

} // namespace di::persona_engine::facilitator
