#include "flow/proxy/stance.hpp"

#include <algorithm>

#include "base/strings.hpp"

namespace di::flow::proxy {
namespace {

bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/** lower.search(/\b<word>\b/) (JS の \b は ASCII の \w 基準)。 */
std::size_t search_word(const std::string& s, const std::string& word) {
    for (std::size_t p = s.find(word); p != std::string::npos; p = s.find(word, p + 1)) {
        const bool left = p == 0 || !is_word(s[p - 1]);
        const std::size_t e = p + word.size();
        const bool right = e >= s.size() || !is_word(s[e]);
        if (left && right) return p;
    }
    return std::string::npos;
}

} // namespace

std::string build_proxy_stance_prompt(const std::string& issue_title, const std::string& bundle_text) {
    return bundle_text + "\n\n" + "# 論点 (問い)\n" + issue_title + "\n" + "(問いの形でなければ「" + issue_title +
           " は成り立つか / 望ましいか」と読み替える)\n\n" +
           "上の人物の主張と発言だけを材料に、この人物がこの論点に賛成するか反対するかを判定し、ラベルを 1 つだけ返してください (説明不要):\n" +
           "- pro: 発言から賛成すると言える\n" + "- con: 発言から反対すると言える\n" +
           "- neutral: 発言からはどちらとも言えない / 材料が無い\n" + "発言に無い好みを想像で補わない。迷ったら neutral。";
}

std::optional<ProxyStance> parse_proxy_stance(const std::string& text) {
    const std::string lower = str::to_lower(text);
    std::optional<ProxyStance> best;
    std::size_t best_index = std::string::npos;
    for (const char* label : {"neutral", "pro", "con"}) {
        const std::size_t i = search_word(lower, label);
        if (i == std::string::npos) continue;
        if (!best || i < best_index) {  // 同じ位置なら先に並ぶ方 (安定ソート)
            best = label;
            best_index = i;
        }
    }
    return best;
}

ProxyStance judge_proxy_stance(const JudgeProxyStanceArgs& args) {
    const auto warn = args.warn ? args.warn : [](const std::string&) {};
    persona_engine::LLMInvokeArgs req;
    req.prompt = build_proxy_stance_prompt(args.issueTitle, args.bundleText);
    if (args.model && !args.model->empty()) req.model = args.model;
    req.maxTokens = 20;
    const auto result = args.llm ? args.llm->invoke(req) : persona_engine::LLMResult::failure("llm がありません");
    if (!result.ok) {
        warn("代理の立場判定 LLM エラー: " + result.error + " — どちらでもないとして扱う");
        return "neutral";
    }
    auto stance = parse_proxy_stance(result.text);
    if (!stance) {
        warn("代理の立場判定ラベル不明: \"" + str::js_slice(result.text, 0, 40) + "\" — どちらでもないとして扱う");
        return "neutral";
    }
    return *stance;
}

Json to_json(const IssueSides& s) {
    if (s.opposed) return Json{{"opposed", true}, {"pro", s.pro}, {"con", s.con}};
    return Json{{"opposed", false}, {"reason", s.reason}};
}

IssueSides assign_issue_sides(const std::vector<ProxyStance>& stances, const std::vector<std::string>& labels) {
    const auto find = [&](const char* v) {
        auto it = std::find(stances.begin(), stances.end(), v);
        return it == stances.end() ? -1 : static_cast<int>(it - stances.begin());
    };
    const int pro = find("pro");
    const int con = find("con");
    IssueSides out;
    if (pro >= 0 && con >= 0) {
        out.opposed = true;
        out.pro = pro;
        out.con = con;
        return out;
    }
    std::vector<std::string> detail;
    for (std::size_t i = 0; i < stances.size(); ++i) {
        const std::string who = i < labels.size() ? labels[i] : "代理" + std::to_string(i + 1);
        const std::string ja = stances[i] == "pro" ? "賛成" : stances[i] == "con" ? "反対" : "どちらとも言えない";
        detail.push_back(who + "は" + ja);
    }
    out.reason = "本人の発言から見ると" + str::join(detail, "、") + "で、この論点では対立していません。反対役を作らずに結論なしとします。";
    return out;
}

} // namespace di::flow::proxy
