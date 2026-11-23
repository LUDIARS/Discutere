#include "flow/dialectic/conclusion_summary.hpp"

#include "base/strings.hpp"
#include "persona_engine/facilitator/prompts.hpp"

namespace di::flow::dialectic {

namespace fac = persona_engine::facilitator;

std::string build_dialectic_summary_prompt(const DialecticSummaryArgs& args) {
    std::string facts;
    if (!args.unresolvedFacts.empty()) {
        std::vector<std::string> lines;
        for (const auto& f : args.unresolvedFacts) lines.push_back("- " + f);
        facts = "\n# 未解決の事実問題\n" + str::join(lines, "\n") + "\n";
    }
    return std::string("あなたは議論の結論をまとめる進行役です。下の「論点ごとの決着」だけを材料に、議題への結論の要約を書きます。\n\n") +
           "# 議題\n" + args.theme + "\n\n# 論点ごとの決着\n" + args.settlementTable + "\n" + facts + "\n# 守ること\n" +
           "- 決着表に無い事実・主張を足さない。\n" +
           "- 「崩れた根拠」「未決の根拠」と「未討議」の論点は、結論の前提にも根拠にも使わない。\n" +
           "- 【結論なし】【未討議】の論点の命題を、他の論点の結論の前提に置かない。決着しなかったことはそのまま書く。\n" +
           "- 【論破】は残った側の主張を、その限定の範囲で書く。【止揚】は統合案を書き、批准の結果を言い換えない。\n" +
           "- 人の名前は決着表に書かれた事実 (誰の主張か) にだけ付ける。\n" +
           "- 作り手が次に確かめること (未決・結論なしから出てくる問い) を最後に 1〜3 個挙げる。\n\n" +
           "次の JSON 1 個だけを返す (前後に説明やコードフェンスを付けない):\n" +
           "{\"summary\": \"<結論の要約 3〜8 文。次に確かめることを含める>\"}";
}

std::optional<std::string> parse_dialectic_summary(const std::string& text) {
    try {
        const Json parsed = fac::extract_json_object(text);
        if (parsed.is_object() && parsed.contains("summary") && parsed["summary"].is_string()) {
            std::string s = str::trim(parsed["summary"].get<std::string>());
            if (!s.empty()) return s;
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<std::string> generate_dialectic_summary(persona_engine::LLMClient& llm, const DialecticSummaryArgs& args) {
    persona_engine::LLMInvokeArgs call;
    call.prompt = build_dialectic_summary_prompt(args);
    if (args.model && !args.model->empty()) call.model = *args.model;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto result = llm.invoke(call);
        if (!result.ok) {
            if (args.warn) args.warn("結論の要約 LLM エラー: " + result.error + " — 決着表だけで結論にする");
            return std::nullopt;
        }
        if (auto s = parse_dialectic_summary(result.text)) return s;
        if (args.warn) args.warn(attempt == 0 ? "結論の要約 JSON パース失敗 — 1 回だけやり直す" : "結論の要約 JSON パース失敗 — 決着表だけで結論にする");
    }
    return std::nullopt;
}

std::string compose_dialectic_conclusion(const std::optional<std::string>& summary, const std::string& settlement_table,
                                         const std::vector<std::string>& unresolved_facts) {
    std::string out = summary ? *summary : std::string("(要約は作れなかった。論点ごとの決着を結論とする)");
    if (!settlement_table.empty()) out += "\n\n# 論点ごとの決着\n" + settlement_table;
    if (!unresolved_facts.empty()) out += "\n\n# 未解決の事実問題\n- " + str::join(unresolved_facts, "\n- ");
    return out;
}

} // namespace di::flow::dialectic
