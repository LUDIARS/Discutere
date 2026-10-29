#pragma once
// 投票の評価レンズ (respec 03, A4)。移植元 src/flow/vote-lenses.ts。
//
// 同一モデル N 票の強相関 (実質 1 票) を崩すため、投票者ごとに評価観点 (レンズ) を
// 割り当てる。ペルソナは注入しない (中立性は維持し、**観点だけ**分ける)。
// config `flow.voteLenses[]` で上書き可 (既定は下記 3 種)。
// config と vote の双方から参照されるため、依存の少ない単独ファイルに置く。
#include <string>
#include <vector>

#include "base/json.hpp"

namespace di::flow {

struct VoteLens {
    /** レンズ識別子 (logic / grounds / relevance 等)。 */
    std::string key;
    /** 投票プロンプトに差し込む評価観点の指示文。 */
    std::string instruction;
    bool operator==(const VoteLens&) const = default;
};

/** 既定レンズ 3 種 (論理 / 根拠 / テーマ適合)。 */
const std::vector<VoteLens>& DEFAULT_VOTE_LENSES();

Json to_json(const VoteLens& lens);
VoteLens vote_lens_from_json(const Json& value);

} // namespace di::flow
