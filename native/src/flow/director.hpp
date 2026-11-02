#pragma once
// src/flow/director.ts の移植: FlowDirector — 議論フロー (discussion.md) の進行オーケストレータ。
//
// persona-engine を使わず、ターン駆動の同期ループで議論を回す (呼んだスレッドで最後まで進める)。
// run_flow は「進行の台本」(ループ制御・評価・サマリ・収束判定・結論の呼び出し順) のみを持ち、
// 各 step の実体は flow_setup / persona_setup / facilitator_turn / persona_turn に分割 (respec 10)。
// すべての LLM 呼び出しは with_cost_log 経由で llm_call_log に記録される。
// エラーは握り潰さず発話に出す (OVERVIEW §10)。
//
// C++ 版の追加: FlowRunState を渡すと進行状態 (段階・ラウンド・ターン・発話数) を別スレッドから
// 問い合わせられ、request_stop() で次の区切りに FlowStopped を投げて止められる (渡さなければ TS と同じ挙動)。
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/json.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/effect_predict.hpp"
#include "flow/flow_setup.hpp"
#include "flow/investigate.hpp"
#include "flow/personas.hpp"
#include "flow/tags.hpp"
#include "flow/voice_cache.hpp"
#include "flow/vote.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

struct FlowUtteranceRecord {
    std::string id;
    std::string sessionId;
    std::string paperId;
    int round = 0;
    int turn = 0;
    std::string personaId;
    std::string personaName;
    std::string role;
    std::string stance;
    std::string text;
    bool isError = false;
    /** その発話で演じていた憑依ペルソナの露出名 (B, item4)。憑依なしは nullopt。 */
    std::optional<std::string> possessionName;
    /** 会話行為 (respec 05 / PR-B, dialectic エンジン専用)。rounds エンジンの発話は nullopt (DB では NULL)。 */
    std::optional<std::string> act;
    /** 応答先 utterance id (act とセット)。 */
    std::optional<std::string> targetId;
    /** 代理議論のみ: 本人の発言との関係。 */
    std::optional<std::string> claimKind;
    /** 代理議論のみ: 根拠にした本人の発言 id。 */
    std::optional<std::vector<std::string>> evidenceIds;
};

/** TS のオブジェクトと同じキー順 (undefined のフィールドは出さない)。 */
Json to_json(const FlowUtteranceRecord& u);

/** ラウンド投票結果の通知 (item3)。 */
struct VoteEvent {
    int round = 0;
    /** "votes" | "scores" (respec 03, A10)。 */
    std::string kind;
    /** utterance_id ごとの得票数 (scores のときは射影スコア)。挿入順。 */
    std::vector<std::pair<std::string, double>> tally;
    /** 最多得票の utterance_id (世論)。nullopt = 無投票。 */
    std::optional<std::string> winner;
    struct Candidate {
        std::string id;
        std::string personaName;
    };
    std::vector<Candidate> candidates;
};

Json to_json(const VoteEvent& e);

/**
 * 進行状態 (C++ 版の追加)。run_flow の外 (別スレッド) から読む・止める。
 * phase: "setup" | "cast" | "round" | "vote" | "summary" | "conclusion" | "finalize" | "done" | "stopped"
 */
class FlowRunState {
public:
    struct Snapshot {
        std::string phase;
        int round = 0;
        int rounds = 0;
        int turn = 0;
        int utterances = 0;
        bool stopRequested = false;
    };
    Snapshot snapshot() const;
    /** 次の区切り (ターン・評価・サマリの前) で止める。 */
    void request_stop() { stop_.store(true); }
    bool stop_requested() const { return stop_.load(); }

    // director が書く
    void set_phase(std::string phase);
    void set_position(int round, int rounds, int turn);
    void set_utterances(int n);

private:
    mutable std::mutex mutex_;
    std::string phase_ = "setup";
    int round_ = 0;
    int rounds_ = 0;
    int turn_ = 0;
    int utterances_ = 0;
    std::atomic<bool> stop_{false};
};

/** request_stop() で止まったとき run_flow が投げる。発話は永続済みのまま (再議論で回し直せる)。 */
class FlowStopped : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct FlowDirectorDeps {
    /** ユーザ意見取得 (未指定は空配列)。 */
    ListExternalVoicesFn listExternalVoices;
    /** YouTube 検索 (未設定なら YouTube 補完は走らない)。 */
    YoutubeSearchFn youtubeSearch;
    /** 発話後のコールバック (Discord/WebUI への投稿等)。例外は run_flow の外へ出る (TS と同じ)。 */
    std::function<void(const FlowUtteranceRecord&)> onUtterance;
    /** ラウンド投票後のコールバック (item3)。例外は warn のみ。 */
    std::function<void(const VoteEvent&)> onVote;
    /** ログ・警告出力 (未指定は log::info("[flow] ...") / log::warn("[flow/warn] ...")) */
    WarnFn log;
    WarnFn warn;
    /** 未指定は crypto::random_unit。 */
    Rng rng;
    std::optional<std::string> gamesDir;
    /** セッション ID を外部から指定する。既定は uuid_v4。 */
    std::optional<std::string> sessionId;
    /** ラウンド数 (省略時は config.flow.rounds)。1..MAX_ROUNDS にクランプ。 */
    std::optional<double> rounds;
    /** 1 ラウンドあたりターン数 (省略時は config.flow.turnsPerRound)。1..MAX_TURNS_PER_ROUND にクランプ。 */
    std::optional<double> turnsPerRound;
    /** 議論/改善キャストに含める生成済みペルソナの id/name。 */
    std::optional<std::vector<std::string>> personaIds;
    /** 憑依 (B)。既定 true。 */
    bool possess = true;
    /** 確定済みディスカッションペーパー (指定時は investigate を省略)。 */
    std::optional<PaperOverride> paperOverride;
    /** 代理議論の参加者 2 人 (dialectic エンジン用。rounds エンジンは使わない)。 */
    std::optional<std::vector<FlowPersona>> proxyParticipants;
    /** 進行状態 (C++ 版の追加)。 */
    std::shared_ptr<FlowRunState> state;
};

/** 都度指定ラウンド/ターン数の暴走ガード上限 (コスト保護)。 */
inline constexpr int MAX_ROUNDS = 10;
inline constexpr int MAX_TURNS_PER_ROUND = 20;

/** 都度指定値を [1, max] にクランプ。未指定/非有限なら fallback を返す。 */
double clamp_count(std::optional<double> value, double fallback, double max);

struct FlowDirectorResult {
    std::string sessionId;
    std::string paperId;
    std::vector<FlowUtteranceRecord> utterances;
    int rounds = 0;
    std::string conclusion;
    bool concluded = false;
    struct Foresight {
        std::string proposal;
        std::string forecast;
    };
    /** 未来予測フェーズの提案と予測 (foresight/after-flow が付ける)。 */
    std::optional<Foresight> foresight;
};

Json to_json(const FlowDirectorResult& r);

/** ラウンドの世論決定を差し替えるための評価コンテキスト。 */
struct RoundEvalContext {
    std::string theme;
    std::string sessionId;
    std::string paperId;
    int round = 0;
    std::vector<MechanicSummary> mechanics;
    std::vector<FlowTag> tags;
    /** 投票/スコア候補 (ファシリテーター発話・エラー発話を除いた当ラウンドの意見) */
    std::vector<RoundUtterance> candidates;
    persona_engine::LLMClient* llm = nullptr;
    WarnFn warn;
};

/** ラウンド世論決定の戦略 (議論=中立投票 / 改善=機械スコア)。 */
using RoundEvaluator = std::function<VoteResult(const RoundEvalContext&)>;

struct FlowRunOptions : FlowDirectorDeps {
    /** フロー種別 (コストログ / ペーパーに記録)。 */
    std::string flow = "discussion";
    /** 未指定は中立投票 (run_round_vote)。 */
    RoundEvaluator evaluateRound;
};

/** 投票候補の判定 (ファシリテーター・エラー・他ラウンドの発話は除く)。 */
bool is_vote_candidate(const FlowUtteranceRecord& u, int round);

/** flow_utterance テーブルにターン発話を永続化する。 */
void persist_utterance(const FlowUtteranceRecord& u);

/** 議論骨格フローを実行する (discussion.md の 9 ステップ)。 */
FlowDirectorResult run_flow(persona_engine::LLMClient& llm, const std::string& theme, const std::vector<FlowTag>& tags,
                            const FlowRunOptions& options);

/** 議論フローを実行する (世論決定 = 中立投票)。run_flow の薄いラッパ。 */
FlowDirectorResult run_discussion_flow(persona_engine::LLMClient& llm, const std::string& theme,
                                       const std::vector<FlowTag>& tags, const FlowDirectorDeps& deps);

} // namespace di::flow
