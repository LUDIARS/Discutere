#pragma once
// ゲーム単位の声の引き出し (src/flow/user-voices/game-voices.ts の移植、spec/feature/flow/user-voices.md)。
//
// Steam レビューや GLAB の感想は本文にゲーム名を書かないことが多く、議題の語によるキーワード検索では
// 見つからない。取込時に付けた gameSlug (attribution) でゲーム単位に引き、議題の埋め込みがあれば
// ベクトルの近い順、無ければ新しい順に並べる。
#include <optional>
#include <string>
#include <vector>

#include "core/index.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "flow/discussion_paper.hpp"

namespace di::flow::user_voices {

/** 1 ゲームから候補として読む最大件数 (並べ替えの母数)。 */
inline constexpr int GAME_VOICE_CANDIDATE_CAP = 2000;

struct ListGameVoicesArgs {
    core::Core* core = nullptr;
    crawler::sources::AttributionStore* attribution = nullptr;
    std::string workspaceId;
    std::string gameSlug;
    int limit = 0;
    /** 議題の埋め込み (無ければ新しい順)。 */
    std::optional<std::vector<double>> queryVector;
};

/** gameSlug の声を議題に近い順 (ベクトルがあれば) に limit 件返す。 */
std::vector<ContextVoice> list_game_voices(const ListGameVoicesArgs& args);

} // namespace di::flow::user_voices
