#include "flow/dialectic/issues.hpp"

#include <algorithm>
#include <stdexcept>

#include "base/strings.hpp"
#include "flow/mechanic_extract.hpp"

namespace di::flow::dialectic {

std::pair<std::string, std::string> build_decompose_prompt(const DecomposeIssuesArgs& args) {
    std::string system = std::string("あなたは議論設計者です。 与えられた議題とディスカッションペーパーから、") +
                         "「賛成側と反対側が本気で対立できる論点 (争点)」だけを分解します。" +
                         " 単なる確認事項や、誰も反対しない自明な項目は含めません。 返答は JSON 配列のみ。";
    const std::string paper = args.paperMd ? str::trim(*args.paperMd) : "";
    std::string prompt = "# 議題\n" + args.theme + "\n\n" + (paper.empty() ? "" : "# ディスカッションペーパー\n" + paper + "\n\n") +
                         "賛否が本気で割れる論点を " + std::to_string(ISSUES_MIN) + "〜" +
                         std::to_string(std::max(ISSUES_MIN, args.maxIssues)) + " 個を目安に、" +
                         "各 1 文の疑問文で挙げてください。 本当に争点が無い場合は空配列を返して構いません。\n" +
                         "次の JSON 配列だけを返してください (前後に説明やコードフェンスを付けない):\n[\"論点1\", \"論点2\", ...]";
    return {system, prompt};
}

std::vector<std::string> decompose_issues(persona_engine::LLMClient& llm, const DecomposeIssuesArgs& args) {
    auto [system, prompt] = build_decompose_prompt(args);
    persona_engine::LLMInvokeArgs call;
    call.system = system;
    call.prompt = prompt;
    call.model = args.model;
    const auto res = llm.invoke(call);
    if (!res.ok) throw std::runtime_error("論点分解 LLM エラー: " + res.error);
    auto arr = extract_json_array(res.text);
    if (!arr) throw std::runtime_error("論点分解の JSON 解析に失敗");
    std::vector<std::string> issues;
    for (const auto& v : *arr) {
        if (!v.is_string()) continue;
        auto s = str::trim(v.get<std::string>());
        if (!s.empty()) issues.push_back(std::move(s));
    }
    const std::size_t cap = static_cast<std::size_t>(std::max(1, args.maxIssues));
    if (issues.size() > cap) issues.resize(cap);
    return issues;
}

} // namespace di::flow::dialectic
