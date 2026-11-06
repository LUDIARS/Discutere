#pragma once
// Rule fire 時の prompt 組み立て (src/persona-engine/engine/prompt-builder.ts)。
//   system: persona の identity (name + display_name + traits + speech_style)
//   user:   rule.instructions + 議論コンテキスト (JSON) + JSON 応答必須の指示
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "persona_engine/context_provider.hpp"
#include "persona_engine/types.hpp"

namespace di::persona_engine::engine {

/** prompt に載せる外部の声の最大件数 (RAG / §14)。 */
inline constexpr int EXTERNAL_VOICE_TOP_K = 6;
/** prompt に載せる進行役への調整指示の最大件数。 */
inline constexpr int DIRECTIVE_TOP_K = 5;

struct BuildPromptArgs {
    const RuleRow* rule = nullptr;
    const PersonaRow* persona = nullptr;
    std::string workspaceId;
    /** event 発火時の sessionId (tick の場合は nullopt 可) */
    std::optional<std::string> sessionId;
    DiscussionContextProvider* contextProvider = nullptr;
    std::string triggeredBy;
};

struct BuiltPromptContext {
    std::vector<ContextHypothesis> hypotheses;
    std::vector<ContextGap> gaps;
    std::vector<ContextUtterance> utterances;
    /** 議題に関連する外部の生の声。無ければ JSON に出さない。 */
    std::vector<ContextExternalVoice> externalVoices;
};

struct BuiltPrompt {
    std::string system;
    std::string user;
    BuiltPromptContext ctx;
};

Json to_json(const BuiltPromptContext& ctx);

/** 議題 (gap title + description) から検索語を粗く抽出する (RAG retrieval 用)。テスト用に公開。 */
std::vector<std::string> extract_topic_terms(const ContextGap& gap);

BuiltPrompt build_prompt(const BuildPromptArgs& args);

} // namespace di::persona_engine::engine
