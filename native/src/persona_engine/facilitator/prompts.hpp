#pragma once
// Facilitator の LLM プロンプト + JSON 抽出 (src/persona-engine/facilitator/prompts.ts)。
// - expand: 議論が停滞した時、新しい視点の persona を 1 体生成し開口一番を出す
// - converge: persona が十分出揃った時、議論を要約して結論を 1 本出す
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"

namespace di::persona_engine::facilitator {

struct RecentUtterance {
    std::string speaker;  // 表示用 (persona 名 or "参加者")
    std::string content;
};

struct GeneratedPersona {
    std::string name;          // 役割名 (例 "コスト懐疑")
    std::string display_name;  // 人名 (例 "費田 渋")
    std::string description;
    std::vector<std::string> traits;
    std::string speech_style;
    std::string opening;  // 開口一番
};

struct PromptPair {
    std::string system;
    std::string user;
};

/** LLM 応答の JSON を読む上限。 */
JsonLimits llm_json_limits();

/** LLM 応答テキストから最初の '{' 〜 最後の '}' を JSON として読む。無い・壊れていれば std::runtime_error。 */
Json extract_json_object(std::string_view text);

PromptPair build_expand_prompt(const std::string& topic, const std::vector<std::string>& existing_persona_names,
                               const std::vector<RecentUtterance>& recent);
/** 止揚 (アウフヘーベン) 判定。既出 (stocked_summaries) と実質同じなら found=false。 */
PromptPair build_aufhebung_judge_prompt(const std::string& topic, const std::vector<RecentUtterance>& recent,
                                        const std::vector<std::string>& stocked_summaries);
PromptPair build_converge_prompt(const std::string& topic, const std::vector<RecentUtterance>& recent,
                                 const std::optional<std::vector<std::string>>& aufhebungen = std::nullopt,
                                 const std::optional<std::vector<std::string>>& top_opinions = std::nullopt);

} // namespace di::persona_engine::facilitator
