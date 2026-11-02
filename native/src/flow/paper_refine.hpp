#pragma once
// src/flow/paper-refine.ts の移植: ディスカッションペーパー本文の LLM リファイン (議論終了後)。
// 議論ループの後に base ブリーフを議論の成果で書き換える。出力が `# 議題` を欠く / 空なら nullopt (degrade)。
#include <optional>
#include <string>
#include <vector>

#include "flow/effect_predict.hpp"
#include "flow/paper_markdown.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

struct RefinePaperBriefInput {
    /** 進行ログを含まない base ブリーフ md。 */
    std::string baseBodyMd;
    std::string theme;
    std::vector<RoundSummary> rounds;
    std::optional<std::string> conclusion;
    /** コストログ用。 */
    std::string sessionId;
    std::string flow = "discussion";
    std::optional<std::string> model;
    /** 未指定は log::warn。 */
    WarnFn warn;
};

/** リファイン指示プロンプト (golden 用に公開)。 */
std::string build_refine_prompt(const RefinePaperBriefInput& input);

/** base ブリーフを議論の成果で改訂した md を返す。失敗 / 不正出力は nullopt。 */
std::optional<std::string> refine_paper_brief(persona_engine::LLMClient& llm, const RefinePaperBriefInput& input);

} // namespace di::flow
