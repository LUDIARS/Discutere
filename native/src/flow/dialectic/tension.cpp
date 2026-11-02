#include "flow/dialectic/tension.hpp"

#include "base/strings.hpp"

namespace di::flow::dialectic {

namespace {

/** 事実照会の証拠件数上限。 */
constexpr int EVIDENCE_LIMIT = 10;
/** 証拠 1 件の切り詰め長。 */
constexpr std::int64_t EVIDENCE_TRIM = 200;

std::string values_text(const PositionRecord& p) {
    const std::string v = str::join(p.values, " / ");
    return v.empty() ? "(なし)" : v;
}

bool is_az(char c) { return c >= 'a' && c <= 'z'; }
bool is_word(char c) { return is_az(c) || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/** /(^|[^a-z])x([^a-z]|$)/ の一致位置 (一致の開始 = 直前の文字の位置)。 */
std::optional<std::size_t> find_isolated(const std::string& t, char x) {
    for (std::size_t p = 0; p < t.size(); ++p) {
        if (t[p] != x) continue;
        if (p > 0 && is_az(t[p - 1])) continue;
        if (p + 1 < t.size() && is_az(t[p + 1])) continue;
        return p == 0 ? 0 : p - 1;
    }
    return std::nullopt;
}

/** /^x\b/ */
bool starts_with_word(const std::string& t, char x) { return !t.empty() && t[0] == x && (t.size() == 1 || !is_word(t[1])); }

} // namespace

std::string build_classify_prompt(const IssueRecord& issue, const PositionRecord& a, const PositionRecord& b) {
    return "# 論点\n" + issue.title + "\n\n" + "# 対立する 2 つの Position\n" + "A (" + a.stance + "): " + a.claim +
           "\n  価値前提: " + values_text(a) + "\n" + "B (" + b.stance + "): " + b.claim + "\n  価値前提: " + values_text(b) +
           "\n\n" + "この対立の「型」を 1 つだけ選んで、そのラベルのみを返してください (説明不要):\n" +
           "- facts: 事実の真偽で決まる対立 (データ照会で解消できる)\n" + "- values: 価値前提の対立 (収益性 vs 体験 など)\n" +
           "- means: 目標は共有しているが手段が違う\n" + "- tradeoff: 両立不能に見える 2 変数のトレードオフ";
}

std::optional<TensionType> parse_tension_type(const std::string& text) {
    const std::string lower = str::to_lower(text);
    std::optional<TensionType> best;
    std::size_t best_index = std::string::npos;
    for (const char* t : {"facts", "values", "means", "tradeoff"}) {
        const auto idx = lower.find(t);
        if (idx != std::string::npos && (!best || idx < best_index)) {
            best = t;
            best_index = idx;
        }
    }
    return best;
}

TensionType classify_tension(persona_engine::LLMClient& llm, const ClassifyTensionArgs& args) {
    persona_engine::LLMInvokeArgs call;
    call.prompt = build_classify_prompt(args.issue, args.positionA, args.positionB);
    if (args.model && !args.model->empty()) call.model = *args.model;
    call.maxTokens = 100;
    const auto result = llm.invoke(call);
    if (!result.ok) {
        if (args.warn) args.warn("Tension 型分類 LLM エラー: " + result.error + " — values に倒して続行 (degrade)");
        return "values";
    }
    auto type = parse_tension_type(result.text);
    if (!type) {
        if (args.warn)
            args.warn("Tension 型分類ラベル不明: \"" + str::js_slice(result.text, 0, 60) + "\" — values に倒して続行 (degrade)");
        return "values";
    }
    return *type;
}

std::string build_fact_resolve_prompt(const IssueRecord& issue, const PositionRecord& a, const PositionRecord& b,
                                      const std::vector<ContextVoice>& evidence) {
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < evidence.size(); ++i)
        lines.push_back(std::to_string(i + 1) + ". " + str::js_slice(evidence[i].content, 0, EVIDENCE_TRIM) + "（出所: " +
                        evidence[i].source + "）");
    return "# 事実に関する対立\n論点: " + issue.title + "\n" + "A: " + a.claim + "\nB: " + b.claim + "\n\n" +
           "# 証拠 (外部の声)\n" + str::join(lines, "\n") + "\n\n" +
           "証拠はどちらの主張を支持しますか。次のいずれか 1 語のみで答えてください (説明不要):\n" +
           "A / B / none (どちらも支持しない・判定不能)";
}

std::optional<std::string> parse_fact_verdict(const std::string& text) {
    const std::string t = str::to_lower(str::trim(text));
    if (starts_with_word(t, 'a')) return "a";
    if (starts_with_word(t, 'b')) return "b";
    if (str::contains(t, "none") || str::contains(t, "どちらも")) return "none";
    // 応答内の先頭出現で判定 (余分なテキスト混入への耐性)。
    const auto ia = find_isolated(t, 'a');
    const auto ib = find_isolated(t, 'b');
    if (ia && (!ib || *ia < *ib)) return "a";
    if (ib) return "b";
    return std::nullopt;
}

FactResolution resolve_fact_tension(persona_engine::LLMClient& llm, const ResolveFactArgs& args) {
    const auto& issue = args.issue;
    const auto& a = args.positionA;
    const auto& b = args.positionB;
    std::vector<ContextVoice> evidence;
    try {
        if (args.listExternalVoices) evidence = args.listExternalVoices({issue.title, args.theme}, EVIDENCE_LIMIT);
    } catch (const std::exception& e) {
        if (args.warn) args.warn(std::string("事実照会の証拠取得に失敗: ") + e.what());
    }
    if (evidence.empty())
        return {"unresolved_fact", "「" + issue.title + "」の事実対立 (A: " + a.claim + " / B: " + b.claim +
                                       ") は照会可能な証拠が見つからず未解決"};

    persona_engine::LLMInvokeArgs call;
    call.prompt = build_fact_resolve_prompt(issue, a, b, evidence);
    if (args.model && !args.model->empty()) call.model = *args.model;
    call.maxTokens = 100;
    const auto result = llm.invoke(call);
    const std::string n = std::to_string(evidence.size());
    if (!result.ok) {
        if (args.warn) args.warn("事実照会判定 LLM エラー: " + result.error + " — unresolved_fact として続行 (degrade)");
        return {"unresolved_fact", "「" + issue.title + "」の事実対立は判定 LLM 障害により未解決 (証拠 " + n + " 件は取得済み)"};
    }
    const auto verdict = parse_fact_verdict(result.text);
    if (verdict == std::optional<std::string>("a") || verdict == std::optional<std::string>("b")) {
        const auto& supported = *verdict == "a" ? a : b;
        return {"resolved", "証拠 (" + n + " 件) は " + str::to_upper(*verdict) + " を支持: " + supported.claim};
    }
    return {"unresolved_fact", "「" + issue.title + "」の事実対立 (A: " + a.claim + " / B: " + b.claim + ") は証拠 " + n +
                                   " 件でも決着せず未解決"};
}

} // namespace di::flow::dialectic
