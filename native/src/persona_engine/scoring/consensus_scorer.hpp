#pragma once
// 合意スコアラー (FEATURE ③+④, src/persona-engine/scoring/consensus-scorer.ts) — 各意見発話 (AI persona も人間も同一に) を
// AI が評価し、議論の総意が同意する意見には 👍 リアクション + 合意スコアを記録する。
//
// - open な議論 (discussion-of-gap) ごとに、未採点の意見発話 (persona / 人間、ext: と facilitator persona は除外) を
//   最大 8 件集めて 1 回の LLM 呼び出しで合意判定する。
// - 同意された意見は consensus_scores.score に積み、既存 opinion_scores にも加点する。
// - discord_message_map に対応 message があり react があれば 👍 を付ける。
// 人間と AI の発話は完全に同一レイヤで評価する (区別しない)。
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/index.hpp"
#include "persona_engine/facilitator/periodic.hpp"
#include "persona_engine/llm/client.hpp"
#include "persona_engine/types.hpp"

namespace di::persona_engine::scoring {

inline constexpr const char* CONSENSUS_EMOJI = "👍";
/** 1 回の LLM 呼び出しで採点する意見の最大数。 */
inline constexpr int MAX_CANDIDATES = 8;

struct ConsensusReactInput {
    std::string utteranceId;
    std::string channelId;
    std::string messageId;
    std::string emoji;
};

struct ConsensusScorerDeps {
    std::shared_ptr<core::Core> core;
    LLMClientPtr llm;
    double intervalMs = 0;
    /** discord メッセージへ 👍 を付ける (任意)。失敗 (例外) は warn して続ける。 */
    std::function<void(const ConsensusReactInput&)> react;
    /** 省略時は console_logger()。 */
    std::shared_ptr<Logger> logger;
    /** ワークスペース (TS でも未使用。gap から自然に絞られる)。 */
    std::optional<std::string> workspaceId;
    std::optional<std::string> model;
};

struct Verdict {
    double i = 0;
    bool agree = false;
    double score = 0;
    std::optional<std::string> reasoning;
};

/**
 * LLM 応答から最初の '[' 〜 最後の ']' を JSON 配列として読む (TS の /\[[\s\S]*\]/)。
 * 無い・壊れている・配列でない・上限超過は nullopt。要素は i が整数のものだけ。
 */
std::optional<std::vector<Verdict>> parse_verdicts(const std::string& text);

/** 応答 JSON を読む上限。 */
JsonLimits verdict_json_limits();

class ConsensusScorer {
public:
    explicit ConsensusScorer(ConsensusScorerDeps deps);
    ~ConsensusScorer();
    ConsensusScorer(const ConsensusScorer&) = delete;
    ConsensusScorer& operator=(const ConsensusScorer&) = delete;

    void start();
    void stop();
    void score_once();

private:
    struct Candidate {
        std::string utteranceId;
        std::string speakerId;
        std::string content;
        bool human = false;
    };
    struct Discussion {
        std::string sessionId;
        std::string gapId;
    };
    std::vector<Discussion> list_active_discussions();
    std::vector<Candidate> candidates_for(const std::string& session_id);
    std::string gap_topic(const std::string& gap_id);
    void record_score(const Candidate& c, bool agree, std::int64_t score, const std::string& reasoning);
    void score_discussion(const Discussion& d);
    sql::Database& raw() { return deps_.core->raw(); }

    ConsensusScorerDeps deps_;
    facilitator::PeriodicTask timer_;
};

std::shared_ptr<ConsensusScorer> create_consensus_scorer(ConsensusScorerDeps deps);

} // namespace di::persona_engine::scoring
