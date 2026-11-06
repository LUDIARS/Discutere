#include "flow/paper_refine.hpp"

#include "base/log.hpp"
#include "base/strings.hpp"
#include "flow/cost_logger.hpp"

namespace di::flow {

namespace {

/** ラウンドまとめ + 止揚を議論成果テキストにする。 */
std::string build_outcome_text(const std::vector<RoundSummary>& rounds, const std::optional<std::string>& conclusion) {
    std::vector<std::string> parts;
    for (const auto& r : rounds) {
        const std::string summary = str::trim(r.summary);
        std::vector<std::string> lines{"## ラウンド " + std::to_string(r.round), summary.empty() ? "(まとめなし)" : summary};
        if (!r.aufhebung.empty()) lines.push_back("止揚: " + str::join(r.aufhebung, " / "));
        parts.push_back(str::join(lines, "\n"));
    }
    if (conclusion && !str::trim(*conclusion).empty()) parts.push_back("## 結論\n" + str::trim(*conclusion));
    const std::string out = str::join(parts, "\n\n");
    return out.empty() ? "(議論成果なし)" : out;
}

} // namespace

std::string build_refine_prompt(const RefinePaperBriefInput& input) {
    const std::string base = str::trim(input.baseBodyMd);
    return str::join(
        {"あなたはゲーム議論のディスカッションペーパー編集者です。",
         "以下の「現在のブリーフ」を、議論の成果を踏まえて改訂してください。", "", "# 制約",
         "- セクション見出し (# 議題 / # 観点補足 / # ゲームのメカニクス) を維持する。",
         "- 議題の主旨・タイトルは変えない。議論で深まった論点・観点を反映して具体化・整理する。",
         "- メカニクスは議論で重要と分かった項目を加筆/整理してよい (箇条書き `- **名前**: 説明` 形式を維持)。",
         "- 出力は markdown 本文のみ。「# 議論の経過」「# 結論」は付けない (別で付与する)。前置き・コードフェンスも不要。", "",
         "# 現在のブリーフ", base.empty() ? "(空)" : base, "", "# 議論の成果", build_outcome_text(input.rounds, input.conclusion)},
        "\n");
}

std::optional<std::string> refine_paper_brief(persona_engine::LLMClient& llm, const RefinePaperBriefInput& input) {
    const WarnFn warn = input.warn ? input.warn : WarnFn([](const std::string& m) { log::warn(m); });
    auto logged = with_cost_log(llm, {input.flow, input.sessionId, {}, {}, {}, {}, "paper-refine", {}});
    persona_engine::LLMInvokeArgs call;
    call.prompt = build_refine_prompt(input);
    call.model = input.model;
    const auto result = logged.invoke(call);
    if (!result.ok) {
        warn("ペーパーリファイン失敗 (元のブリーフを使用): " + result.error);
        return std::nullopt;
    }
    const std::string text = str::trim(result.text);
    // 議題見出しを欠く / 空なら派生 (markdown_to_paper_draft) が壊れるので degrade。
    if (text.empty() || !str::contains(text, "# 議題")) {
        warn("ペーパーリファイン出力が不正 (# 議題 欠落)。元のブリーフを使用");
        return std::nullopt;
    }
    return text;
}

} // namespace di::flow
