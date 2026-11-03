#pragma once
// src/flow/sparring.ts の移植: 壁打ちフロー (sparring.md / OVERVIEW §9)。
//
// ユーザターン起点の対話フロー。ユーザの不定期発話に対し議論側が応答する。
// ラウンド/ターンの固定制限なし。flow.sparringMaxTurns を暴走ガード上限とする。
// 「まとめて」でラウンド締め (続行可)、「おわり」でフロー終了 (結論まとめ)。
//
// 議論骨格は再実装せず T2/T3 の部品 (ディスカッションペーパー / ペルソナ / サマリ / 結論) を流用する。
//
// C++ 版: start / submit_user は呼んだスレッドで同期に進める。同じセッションへの呼び出しは
// 内部の mutex で直列化する (Discord の返信と Web の say が同時に来ても状態が壊れない)。
// LLM は deps.llm (shared_ptr) をセッションが持つので、呼び出し側より長く生きてよい。
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "flow/director.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/investigate.hpp"
#include "flow/personas.hpp"
#include "flow/tags.hpp"
#include "flow/voice_cache.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

struct SparringDeps {
    persona_engine::LLMClientPtr llm;
    ListExternalVoicesFn listExternalVoices;
    YoutubeSearchFn youtubeSearch;
    /** 1 ユーザ発話に対する AI 応答数 (既定 1)。 */
    std::optional<int> responsesPerTurn;
    std::function<void(const FlowUtteranceRecord&)> onUtterance;
    WarnFn log;
    WarnFn warn;
    Rng rng;
    std::optional<std::string> gamesDir;
    /**
     * 壁打ち相手に充てるプールペルソナの id または name (G)。
     * 解決できたものを相手として使う。未指定/未解決なら従来の生成ペルソナ。
     */
    std::vector<std::string> opponentPersonaIds;
};

/** submit_user の返り値。kind: "responses" | "round-summary" | "ended"。 */
struct SparringTurnResult {
    std::string kind;
    // kind = "responses"
    std::optional<FlowUtteranceRecord> user;
    std::vector<FlowUtteranceRecord> utterances;
    // kind = "round-summary"
    int round = 0;
    std::string summary;
    std::vector<std::string> aufhebung;
    // kind = "ended"
    /** "command" | "guard" */
    std::string reason;
    std::string conclusion;
    bool concluded = false;
};

/** TS の判別共用体と同じ形 (kind ごとのキーだけを出す)。 */
Json to_json(const SparringTurnResult& r);

/**
 * 壁打ちセッション。start() で初期化し、submit_user() でユーザ発話ごとに進める。
 * ユーザは不定期に submit_user を呼べる (固定ターン待ちをしない)。
 */
class SparringSession {
public:
    SparringSession(std::string theme, std::vector<FlowTag> tags, SparringDeps deps);

    const std::string& session_id() const { return session_id_; }
    std::string paper_id() const;
    const std::string& theme() const { return theme_; }
    bool is_ended() const { return ended_.load(); }

    /** 調査 + ペーパー + ペルソナを初期化する (LLM 議論はまだ起こさない)。 */
    void start();

    /** ユーザ発話を 1 件受け取り、コマンド or 通常応答を処理する。start 前は std::logic_error。 */
    SparringTurnResult submit_user(const std::string& text);

private:
    FlowUtteranceRecord record_user(const std::string& text);
    std::optional<FlowUtteranceRecord> generate_response();
    SparringTurnResult close_round();
    SparringTurnResult finalize(const std::string& reason);
    WarnFn warn_fn() const;
    Rng rng_fn() const;

    const std::string session_id_;
    const std::string theme_;
    const std::vector<FlowTag> tags_;
    const SparringDeps deps_;

    mutable std::mutex mutex_;
    bool started_ = false;
    std::string paper_id_;
    DiscussionPaper paper_;
    std::vector<FlowPersona> personas_;
    std::vector<std::string> synthetic_opinions_;
    std::vector<NamedUtterance> round_utterances_;
    std::vector<FlowUtteranceRecord> all_utterances_;
    std::vector<std::string> all_aufhebung_;
    int round_ = 1;
    int ai_turn_count_ = 0;
    std::atomic<bool> ended_{false};
    std::optional<InvestigateResult> investigation_;
};

} // namespace di::flow
