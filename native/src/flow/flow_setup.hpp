#pragma once
// src/flow/flow-setup.ts の移植: フローセッションのセットアップ (discussion.md step 1〜2)。
//   [1] 調査 (investigate + メカニクス LLM 増補) — 確定ペーパーがあれば省略
//   [2] ディスカッションペーパー初期化 (body_md 正本化 + persist_paper)
#include <optional>
#include <string>
#include <vector>

#include "flow/discussion_paper.hpp"
#include "flow/effect_predict.hpp"
#include "flow/investigate.hpp"
#include "flow/voice_cache.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

struct DebatabilitySummary {
    bool debatable = true;
    int armableBothCount = 0;
};

/** 人間が調整・承認した確定ペーパーの上書き値 (investigate の出力を置き換える)。 */
struct PaperOverride {
    std::vector<MechanicSummary> mechanics;
    std::string supplement;
    std::optional<std::string> bodyMd;
    std::optional<std::vector<std::string>> issues;
    std::optional<DebatabilitySummary> debatability;
};

struct FlowSetupArgs {
    std::string theme;
    std::vector<FlowTag> tags;
    std::string flow;
    std::string sessionId;
    /** 発話時 RAG のセッションキャッシュ (メカニクス増補の材料 lookup に使う)。必須。 */
    VoiceCache* voiceCache = nullptr;
    YoutubeSearchFn youtubeSearch;
    std::optional<std::string> gamesDir;
    std::optional<PaperOverride> paperOverride;
    WarnFn log;
    WarnFn warn;
};

struct FlowSetupResult {
    DiscussionPaper paper;
    std::string paperId;
    /** investigate の生結果 (確定ペーパー経路・議論フローでは nullopt)。 */
    std::optional<InvestigateResult> investigation;
};

/** 調査 + ペーパー初期化を行い、永続済みの DiscussionPaper を返す。 */
FlowSetupResult setup_flow_paper(persona_engine::LLMClient& llm, const FlowSetupArgs& args);

} // namespace di::flow
