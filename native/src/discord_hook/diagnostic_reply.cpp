#include "discord_hook/diagnostic_reply.hpp"

#include <map>

#include "base/strings.hpp"
#include "design_diagnostic/contracts.hpp"

namespace di::discord_hook {

using design_diagnostic::DIAGNOSTIC_LIMITS;
using design_diagnostic::DiagnosticError;

Json DiagnosticReply::body() const { return Json{{"content", content}, {"allowed_mentions", allowed_mentions_none()}}; }

DiagnosticReply diagnostic_reply(const Json& result, const std::string& filename) {
    const std::string json = json_stringify_pretty(result, 2);
    if (json.size() > DIAGNOSTIC_LIMITS.resultBytes) throw DiagnosticError("result_too_large", "Result attachment exceeds byte limit");
    DiagnosticReply r;
    if (str::js_length(json) <= 1'850) {
        r.content = "```json\n" + json + "\n```";
        return r;
    }
    r.content = "logic_differences / design_gap";
    r.file = FileUpload{filename, json, "application/json"};
    return r;
}

std::string diagnostic_failure(const std::exception* error) {
    const auto* d = dynamic_cast<const DiagnosticError*>(error);
    std::string code = d ? d->code() : std::string("llm_failed");
    static const std::map<std::string, std::string> reasons{
        {"invalid_input", "入力の型・長さ・ID・単位・確率を確認してください。"},
        {"use_level_check", "playModel による場面・予測幅の診断には /level-check を使ってください。"},
        {"llm_unavailable", "LLM が設定されていません。"},
        {"llm_failed", "LLM の単発診断に失敗しました。"},
        {"llm_timeout", "LLM の単発診断が時間内に完了しませんでした。"},
        {"invalid_response", "LLM のJSON結果または根拠参照が不正です。"},
        {"result_too_large", "診断結果が回答サイズの上限を超えました。"},
    };
    auto it = reasons.find(code);
    return "診断失敗 (" + code + "): " + (it == reasons.end() ? std::string("undefined") : it->second);
}

} // namespace di::discord_hook
