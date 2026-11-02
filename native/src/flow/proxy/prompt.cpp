#include "flow/proxy/prompt.hpp"

namespace di::flow::proxy {

std::vector<std::string> proxy_prompt_lines(const FlowPersona& persona) {
    if (!persona.proxy) return {};
    return {
        persona.proxy->bundleText,
        "",
        "# 論拠ごとに申告すること (代理の決まり)",
        "- claimKind: 本人の発言との関係を次の 1 つで書く",
        "  - sourceQuote: 本人の発言をそのまま引く (引く部分を「」で囲み data に入れる)",
        "  - groundedParaphrase: 本人の発言に基づいて言い換える",
        "  - simulationInference: 本人の傾向からの推測 (本人はそう言っていない)",
        "  - unknown: 材料が無くて分からない",
        "- evidenceIds: 根拠にした本人の発言の id の配列 (例 [\"E2\",\"E5\"])。sourceQuote / groundedParaphrase では必須。",
        "  上に無い id を書かない。推測を本人の発言として書かない。",
        "",
    };
}

std::string proxy_ground_fields(const FlowPersona& persona) {
    return persona.proxy
               ? ", \"claimKind\": \"<sourceQuote | groundedParaphrase | simulationInference | unknown>\", \"evidenceIds\": [\"<E番号>\"]"
               : "";
}

} // namespace di::flow::proxy
