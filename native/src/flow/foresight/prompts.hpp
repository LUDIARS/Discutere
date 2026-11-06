#pragma once
// 未来予測フェーズの prompt 組み立て (pure。src/flow/foresight/prompts.ts の移植)。
// イノベーション理論を「適用条件つきの道具」として渡し、提案 → 改革派/保守派の議論 → 予測 の 3 段の prompt を返す。
// LLM 呼び出しは phase (別担当) が行う。
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace di::flow::foresight {

/** "reform" | "conservative" */
using ForesightCamp = std::string;
/** "conditions" | "refutation" | "verification" */
using CampStage = std::string;

struct ForesightLine {
    std::string personaName;
    ForesightCamp camp;
    std::string text;
};

struct ForesightVoice {
    std::string content;
    std::string source;
};

/** 未来予測に渡す本議論の文脈 (ペーパー本文 + 集めた外部の声)。 */
struct ForesightContext {
    std::string paperMd;
    std::vector<ForesightVoice> voices;
};

struct PromptPair {
    std::string system;
    std::string prompt;
};

/** ペーパー本文を prompt に載せる上限 (字、UTF-16 単位)。 */
inline constexpr std::size_t CONTEXT_PAPER_LIMIT = 6000;
/** prompt に載せる外部の声の件数。 */
inline constexpr std::size_t CONTEXT_VOICE_LIMIT = 8;

/** 本議論の文脈ブロック (ペーパー本文 + 外部の声)。無ければ空文字。 */
std::string context_block(const std::optional<ForesightContext>& context);

/** 提案 prompt: 前提事実 → 理論の当てはめ → 破壊可能な一手 + 起きること + リスク。 */
PromptPair build_proposal_prompt(std::string_view theme, std::string_view conclusion,
                                 const std::optional<ForesightContext>& context = std::nullopt);

/** 陣営の立場行。本議論での賛否とは独立に、提案された一手について立つ。 */
std::string camp_stance_line(std::string_view camp);
/** 往復の段階ごとの論点。 */
std::string camp_stage_line(std::string_view stage);

struct CampTurnArgs {
    std::string theme;
    std::string proposal;
    std::string personaName;
    ForesightCamp camp;
    std::vector<ForesightLine> lines;
    std::optional<CampStage> stage;
    std::vector<std::string> ownStatements;
};
/** 議論ターン prompt: 改革派 / 保守派として、段階ごとの論点で提案と帰結を論じる。 */
PromptPair build_camp_turn_prompt(const CampTurnArgs& args);

struct ForecastArgs {
    std::string theme;
    std::string conclusion;
    std::string proposal;
    std::vector<ForesightLine> lines;
    std::optional<ForesightContext> context;
};
/** 予測 prompt: 未解決の前提 + 3 / 5 / 10 年時点の条件付きシナリオ + ビジネス分析。 */
PromptPair build_forecast_prompt(const ForecastArgs& args);

} // namespace di::flow::foresight
