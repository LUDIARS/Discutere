#pragma once
// One conversation-only invocation with bounded timeout and safe error disclosure (src/design-diagnostic/invoke.ts)。
//
// LLM 呼び出しは別スレッドで行い、DIAGNOSTIC_LIMITS.timeoutMs で打ち切る (TS の Promise.race と同じ)。
// 打ち切った呼び出しは裏で終わるまで走るので、同時に走らせる数に上限を置く (移植で追加。超えたら llm_unavailable)。
// エラー文言に LLM バックエンドのエラー内容を含めない。
#include <chrono>
#include <optional>
#include <string>

#include "persona_engine/llm/client.hpp"

namespace di::design_diagnostic {

struct DiagnosticDependencies {
    persona_engine::LLMClientPtr llm;
    std::optional<std::string> model;
};

struct DiagnosticPrompt {
    std::string system;
    std::string prompt;
};

/** 同時に走らせる診断の LLM 呼び出しの上限 (打ち切った後も終わるまで数える)。 */
inline constexpr int kMaxConcurrentDiagnostics = 4;

/**
 * 診断を 1 回呼び、応答本文を返す。失敗は DiagnosticError:
 * llm_unavailable (LLM 未設定・同時実行の上限) / llm_timeout / llm_failed (例外・ok=false)。
 * timeout は省略時 DIAGNOSTIC_LIMITS.timeoutMs (テスト用に短くできる)。
 */
std::string invoke_diagnostic(const DiagnosticPrompt& prompt, const DiagnosticDependencies& deps,
                              std::optional<std::chrono::milliseconds> timeout = std::nullopt);

} // namespace di::design_diagnostic
