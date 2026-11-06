#pragma once
// @implements DI-EXTERNAL-DISCUSSION
// 外部会話 (Actio 等) への参加の提案を作る (src/external-discussion/service.ts)。
// 提案を返すだけで、送信と人間によるタスク承認は呼び出し側 (Actio) が持つ。
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "base/json.hpp"
#include "external_discussion/contracts.hpp"
#include "external_discussion/store.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::external_discussion {

/** LLM の応答 JSON を読む上限 (応答の text は 1200 文字までなので十分に大きい)。 */
JsonLimits discussion_response_json_limits();

/** LLM に渡す system (stance は "pro" | "con")。テスト用に公開。 */
std::string build_discussion_system(const std::string& stance);
/** LLM に渡す prompt (会話を引用データとして JSON にする)。テスト用に公開。 */
std::string build_discussion_prompt(const ConversationInput& input);

class ExternalDiscussionService {
public:
    ExternalDiscussionService(std::shared_ptr<ParticipationStore> store, persona_engine::LLMClientPtr llm, std::string model,
                              std::function<std::int64_t()> now = {}, std::function<double()> rng = {});
    /** 生成に失敗したら例外 (TS の throw と同じ。試行は fail で解放する)。 */
    ParticipationResult consider(const ConversationInput& input);

private:
    std::shared_ptr<ParticipationStore> store_;
    persona_engine::LLMClientPtr llm_;
    std::string model_;
    std::function<std::int64_t()> now_;
    std::function<double()> rng_;
};

} // namespace di::external_discussion
