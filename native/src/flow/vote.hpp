#pragma once
// src/flow/vote.ts の移植: 投票フェーズ (discussion.md step 6, respec 03)。
// flow.voterCount 人の中立投票者がラウンドの発話から最も的を射た意見を 1 つ選ぶ。
// 投票者ごとに評価レンズを巡回割当し、候補の提示順を rng シャッフルし、番号回答も受理し、同点は rng くじ。
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "flow/effect_predict.hpp"
#include "flow/personas.hpp"
#include "flow/vote_lenses.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

struct RoundUtterance {
    std::string id;
    std::string personaName;
    std::string text;
};

struct VoteResult {
    /** "votes" | "scores" (未指定は votes)。 */
    std::optional<std::string> kind;
    /** utterance_id ごとの得票数 (scores のときは射影スコア)。キーは JS のオブジェクトと同じ挿入順。 */
    std::vector<std::pair<std::string, double>> tally;
    std::optional<std::string> winner;
    /** 最多得票シェア。有効票 0 などで算出不能なら nullopt (JSON では null)。 */
    std::optional<double> winnerShare;
};
Json to_json(const VoteResult& r);

/** 投票回答を候補 ID に解決する (完全一致 → 部分一致 → 提示順の番号回答)。 */
std::optional<std::string> parse_vote_id(const std::string& text, const std::vector<std::string>& valid_ids,
                                         const std::vector<std::string>& presented = {});

struct VoteWinner {
    std::optional<std::string> winner;
    double maxVotes = 0;
};
/** 最多得票の決定。同点候補は id 昇順に並べてから rng で引く。 */
VoteWinner pick_vote_winner(const std::vector<std::pair<std::string, double>>& tally, const Rng& rng);

struct RunRoundVoteArgs {
    std::string theme;
    std::string sessionId;
    int round = 0;
    int voterCount = 0;
    std::vector<RoundUtterance> utterances;
    /** 評価レンズ。未指定は DEFAULT_VOTE_LENSES。 */
    std::optional<std::vector<VoteLens>> lenses;
    /** 候補シャッフル・同点くじ用。未指定は crypto::random_unit。 */
    Rng rng;
    /** 未指定は log::warn。 */
    WarnFn warn;
};

std::string build_vote_prompt(const std::string& theme, const std::vector<RoundUtterance>& utterances, const VoteLens& lens);
/** ラウンド投票を実行し、vote テーブルに記録して結果を返す。 */
VoteResult run_round_vote(persona_engine::LLMClient& llm, const RunRoundVoteArgs& args);

} // namespace di::flow
