#pragma once
// src/flow/paper-review-enrich.ts の移植: 振り分けたペーパー調整指示 (paper_review_intent) に、種類ごとの材料を足して
// apply_paper_edit へ渡す指示文を組み立てる。
//   - adjust        : 返信本文をそのまま指示にする。
//   - voices        : 指定語 (無ければ議題) でクロール → 取込し、集めた外部の声を根拠として渡す。
//   - similar_games : 挙げたゲームのメカニクスを data/games から引き、代替 (参考) として渡す。
//                     既存データに無いゲームは LLM の一般知識で補うよう指示する。
// クロール / 検索 / メカニクス取得は注入境界 (テストで差し替え可)。失敗は warn して続行する。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "flow/effect_predict.hpp"
#include "flow/investigate.hpp"
#include "flow/paper_review_intent.hpp"
#include "flow/tags.hpp"
#include "flow/voice_cache.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow {

/** 類似ゲームとして引くゲーム数の上限。 */
inline constexpr std::size_t kMaxSimilarGames = 5;
/** 1 ゲームあたり指示に載せるメカニクス数の上限。 */
inline constexpr std::size_t kMaxMechanicsPerGame = 8;

/** 指定語でクロール → KG 取込し、取り込んだ件数を返す (失敗は例外)。 */
using PaperIntentCrawlFn = std::function<int(const std::string& query)>;
using InvestigateFn = std::function<InvestigateResult(const InvestigateArgs&)>;

struct PaperIntentDeps {
    /** 議論の議題 (検索語が無い時の既定)。 */
    std::string theme;
    std::vector<FlowTag> tags;
    std::optional<std::string> gamesDir;
    /** 収集済みの外部の声検索 (RAG)。無ければ外部の声は集められない。 */
    ListExternalVoicesFn listExternalVoices;
    /** 根拠段落の要約に使う LLM (nullptr なら箇条書き)。 */
    persona_engine::LLMClient* llm = nullptr;
    /** 未指定ならクロールせず収集済みの声だけを使う。 */
    PaperIntentCrawlFn crawl;
    /** メカニクス取得 (テスト用差し替え)。既定は investigate_theme。 */
    InvestigateFn investigate;
    WarnFn warn;
};

struct PaperIntentInstruction {
    /** apply_paper_edit へ渡す指示。nullopt なら反映しない (材料が見つからなかった等)。 */
    std::optional<std::string> instruction;
    /** スレッドへ出す処理結果の一言。 */
    std::string notice;
};
Json to_json(const PaperIntentInstruction& r);

/** 振り分けた指示に材料を足し、apply_paper_edit 用の指示文とスレッド通知を返す。 */
PaperIntentInstruction build_intent_instruction(const PaperReviewIntent& intent, const PaperIntentDeps& deps);

} // namespace di::flow
