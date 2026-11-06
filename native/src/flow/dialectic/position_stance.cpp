#include "flow/dialectic/position_stance.hpp"

#include "base/strings.hpp"

namespace di::flow::dialectic {

std::string build_stance_prompt(const IssueRecord& issue, const std::string& claim, const std::optional<std::string>& qualifier) {
    return "# 論点 (問い)\n" + issue.title + "\n" + "(問いの形でなければ「" + issue.title +
           " は成り立つか / 十分か」と読み替える)\n\n" + "# 主張\n" + claim + "\n限定: " + qualifier.value_or("(明示なし)") +
           "\n\n" +
           "この主張は論点に対して「はい」と答えているか「いいえ」と答えているかを判定し、ラベルを 1 つだけ返してください (説明不要):\n" +
           "- yes: 論点を肯定している (成り立つ / 十分だ / 正解だ)\n" +
           "- no: 論点を否定している (成り立たない / 不十分だ / 正解ではない)\n" + "- unclear: どちらとも言えない";
}

StanceAnswer parse_stance_answer(const std::string& text) {
    const std::string lower = str::to_lower(text);
    std::string best = "unclear";
    std::size_t best_index = std::string::npos;
    for (const char* label : {"yes", "no", "unclear"}) {
        const auto idx = lower.find(label);
        // 同じ位置なら先に並んだ方 (安定ソート)。
        if (idx != std::string::npos && (best_index == std::string::npos || idx < best_index)) {
            best = label;
            best_index = idx;
        }
    }
    return best;
}

StanceAnswer expected_answer(const std::string& stance) { return stance == "pro" ? "yes" : "no"; }

bool is_reversed(const std::string& stance, const StanceAnswer& answer) {
    return answer != "unclear" && answer != expected_answer(stance);
}

StanceAnswer judge_position_stance(persona_engine::LLMClient& llm, const JudgePositionStanceArgs& args) {
    persona_engine::LLMInvokeArgs call;
    call.prompt = build_stance_prompt(args.issue, args.claim, args.qualifier);
    if (args.model && !args.model->empty()) call.model = *args.model;
    call.maxTokens = 50;
    const auto result = llm.invoke(call);
    if (!result.ok) {
        if (args.warn) args.warn("定立の向きの判定 LLM エラー: " + result.error + " — 作り直さずに続行 (degrade)");
        return "unclear";
    }
    return parse_stance_answer(result.text);
}

std::string stance_feedback(const std::string& stance, const std::string& previous_claim) {
    const std::string side = stance == "pro" ? "賛成" : "反対";
    const std::string want = stance == "pro" ? "論点に「はい」と答える (肯定する)" : "論点に「いいえ」と答える (否定する)";
    return "前回の主張「" + previous_claim + "」は【" + side + "】の立場と逆向きでした。" + want +
           "主張にしてください。相手陣営より強い肯定・否定にすり替えないこと。";
}

} // namespace di::flow::dialectic
