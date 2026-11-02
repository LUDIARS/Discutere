#pragma once
// 外部取り込みデータの要約器 (id=67, src/crawler/sources/summarize.ts)。
// 長文 source (website 記事) を LLM で要約し、ExternalUtterance.summary に詰める。
// 議論コンテキスト参照はこの要約を使ってトークンを節約し、raw 全文 (content) は importer 側で raw-store に退避する 2 層化。
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "crawler/sources/types.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::crawler::sources {

class Summarizer {
public:
    virtual ~Summarizer() = default;
    /** text を要約。失敗時は nullopt (呼び出し側は raw を使う)。 */
    virtual std::optional<std::string> summarize(const std::string& text) = 0;
};

struct LlmSummarizerOptions {
    /** モデル上書き */
    std::optional<std::string> model;
    /** 目安の要約字数 (既定 400) */
    std::optional<double> targetChars;
    /** LLM に渡す入力の上限字数 (既定 12000) */
    std::optional<double> inputCap;
};

/** LLMClient を要約器に変換する (system プロンプトは TS と同じ文言)。 */
std::shared_ptr<Summarizer> create_llm_summarizer(persona_engine::LLMClientPtr llm, const LlmSummarizerOptions& opts = {});

/** 要約器の system プロンプト (golden 用に公開)。 */
std::string summarizer_system_prompt(double target_chars);

struct SummarizeOptions {
    /** これ未満の content は要約しない (短文 source = comment はそのまま)。既定 600 */
    std::optional<double> minChars;
    /** 1 件要約するごとの進捗 (累計件数) */
    std::function<void(int)> onItem;
};

/**
 * items を走査して長文のものに要約を付与する (mutate)。付与した件数を返す。
 * 既に summary がある / content が短い / 要約失敗 のものは raw のまま。
 */
int summarize_items(std::vector<ExternalUtterance>& items, Summarizer& summarizer, const SummarizeOptions& opts = {});

} // namespace di::crawler::sources
