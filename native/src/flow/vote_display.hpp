#pragma once
// src/flow/vote-display.ts の移植: 投票結果の表示整形 (respec 03, A10 — tally の意味分離)。
//
// discord-live / slack-live の得票可視化が共有する純関数。
// kind="votes" は「n 票」、kind="scores" (improvement の design_gap 射影) は
// 「スコア x.xx (design_gap 適合)」表記にし、機械スコアを票数として見せない。
// トランスポート非依存 (絵文字/リアクション付与は各アダプタ側)。
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace di::flow {

/** Pick<VoteEvent, "kind" | "tally" | "winner">。VoteEvent からも作れる。 */
struct VoteDisplay {
    /** "votes" | "scores" */
    std::string kind;
    /** 挿入順 (VoteEvent / VoteResult の tally と同じ)。 */
    std::vector<std::pair<std::string, double>> tally;
    std::optional<std::string> winner;
};

/** この評価種別で 👍 (得票リアクション) を枚数積みしてよいか。scores は 🏆 のみ。 */
bool should_stack_vote_reactions(const std::string& kind);

/** 可視化すべき結果があるか (votes: 有効票あり / scores: winner あり)。 */
bool has_visible_vote_result(const VoteDisplay& e);

/**
 * 得票/スコア集計のランキング行 (降順) を組み立てる。
 * votes は正の得票のみ、scores は全候補 (負値もあり得る) を降順で出す (同点は tally の順を保つ)。
 */
std::vector<std::string> build_vote_ranking_lines(const VoteDisplay& e,
                                                  const std::function<std::string(const std::string&)>& name_of);

/** 集計投稿のタイトル行。scores は「投票」でなく「機械スコア」と明示する。 */
std::string vote_notice_title(int round, const std::string& kind);

} // namespace di::flow
