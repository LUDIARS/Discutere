#include "crawler/sources/summarize.hpp"

#include <cmath>

#include "base/json.hpp"
#include "base/strings.hpp"

namespace di::crawler::sources {

std::string summarizer_system_prompt(double target_chars) {
    return std::string("あなたはゲーム議論データの要約器です。 与えられた記事 (レビュー / 考察) を、") +
           " 後段の議論の素として使える形に日本語で簡潔に要約してください。" + " 主張・評価・具体例・賛否の要点だけを残し、 約 " +
           js_number_to_string(target_chars) + " 字以内。" + " 前置き・見出し・箇条書き記号は付けず、 地の文で書く。";
}

namespace {

/** slice の位置 (JS の ToIntegerOrInfinity と同じく小数は切り捨て)。 */
std::int64_t slice_pos(double v) {
    if (std::isnan(v)) return 0;
    if (v > 9e15) return static_cast<std::int64_t>(9e15);
    if (v < -9e15) return static_cast<std::int64_t>(-9e15);
    return static_cast<std::int64_t>(std::trunc(v));
}

class LlmSummarizer : public Summarizer {
public:
    LlmSummarizer(persona_engine::LLMClientPtr llm, LlmSummarizerOptions opts)
        : llm_(std::move(llm)),
          model_(std::move(opts.model)),
          target_chars_(opts.targetChars.value_or(400)),
          input_cap_(opts.inputCap.value_or(12000)),
          system_(summarizer_system_prompt(target_chars_)) {}

    std::optional<std::string> summarize(const std::string& text) override {
        const std::string trimmed = str::trim(text);
        if (trimmed.empty()) return std::nullopt;
        persona_engine::LLMInvokeArgs args;
        args.prompt = str::js_slice(trimmed, 0, slice_pos(input_cap_));
        args.system = system_;
        args.model = model_;
        const auto res = llm_->invoke(args);
        if (!res.ok) return std::nullopt;
        const std::string s = str::trim(res.text);
        if (s.empty()) return std::nullopt;
        // 暴走防止 (目安字数の 2 倍で安全側にカット)。
        if (static_cast<double>(str::js_length(s)) > target_chars_ * 2) return str::js_slice(s, 0, slice_pos(target_chars_ * 2 - 1)) + "…";
        return s;
    }

private:
    persona_engine::LLMClientPtr llm_;
    std::optional<std::string> model_;
    double target_chars_;
    double input_cap_;
    std::string system_;
};

} // namespace

std::shared_ptr<Summarizer> create_llm_summarizer(persona_engine::LLMClientPtr llm, const LlmSummarizerOptions& opts) {
    return std::make_shared<LlmSummarizer>(std::move(llm), opts);
}

int summarize_items(std::vector<ExternalUtterance>& items, Summarizer& summarizer, const SummarizeOptions& opts) {
    const double min_chars = opts.minChars.value_or(600);
    int done = 0;
    for (auto& it : items) {
        if (it.summary && !str::trim(*it.summary).empty()) continue;
        if (static_cast<double>(str::js_length(it.content)) < min_chars) continue;
        auto s = summarizer.summarize(it.content);
        if (s && !s->empty()) {
            it.summary = *s;
            done += 1;
            if (opts.onItem) opts.onItem(done);
        }
    }
    return done;
}

} // namespace di::crawler::sources
