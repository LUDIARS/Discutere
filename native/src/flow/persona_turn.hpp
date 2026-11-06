#pragma once
// src/flow/persona-turn.ts の移植: ペルソナ 1 ターン実行 (discussion.md step 5, respec 10 の runFlow 分割)。
// ペーパー組み立て → LLM invoke → 発話レコード化。stance はセッション固定 (personaStance で導出)。
// 永続・通知は呼び出し側 director が行う (onUtterance / persistUtterance の順序維持)。
#include <optional>
#include <string>
#include <vector>

#include "flow/director.hpp"
#include "flow/discussion_paper.hpp"
#include "flow/effect_predict.hpp"
#include "flow/investigate.hpp"
#include "flow/personas.hpp"
#include "flow/voice_cache.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

struct PersonaTurnArgs {
    FlowPersona persona;
    int round = 0;
    int turn = 0;
    const DiscussionPaper* paper = nullptr;
    std::string sessionId;
    std::string paperId;
    /** フロー種別 (コストログ用)。 */
    std::string flow = "discussion";
    /** 当ラウンドのこれまでの発話 (プロンプトの可変部)。 */
    std::vector<NamedUtterance> roundUtterances;
    /** 発話時 RAG のセッションキャッシュ (全ペルソナ共有)。必須。 */
    VoiceCache* voiceCache = nullptr;
    /** investigate の生結果 (YouTube 補完の声。確定ペーパー経路は nullptr)。 */
    const InvestigateResult* investigation = nullptr;
    /** 機密タグ用の想定意見 (synthetic)。 */
    std::vector<std::string> syntheticOpinions;
    WarnFn log;
    WarnFn warn;
};

/**
 * 1 ターンを実行し発話レコードを返す。空応答は nullopt (発話に数えない)。
 * LLM エラーは isError=true のレコードとして返す (握り潰さない)。paper / voiceCache が無ければ std::invalid_argument。
 */
std::optional<FlowUtteranceRecord> run_persona_turn(persona_engine::LLMClient& llm, const PersonaTurnArgs& args);

} // namespace di::flow
