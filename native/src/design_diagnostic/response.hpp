#pragma once
// Accept only bounded structured comparisons with citations to actual supplied text (src/design-diagnostic/response.ts)。
// LLM の応答は外部入力: バイト数 (DIAGNOSTIC_LIMITS.responseBytes) を確かめてから上限つきで JSON として読む。
// 失敗は DiagnosticError("invalid_response", "<path>: <reason>")。
#include <string>
#include <vector>

#include "design_diagnostic/contracts.hpp"
#include "design_diagnostic/documents.hpp"

namespace di::design_diagnostic {

/** 応答 JSON の要素数・深さの上限 (大きさは responseBytes)。 */
inline constexpr std::size_t kResponseMaxElements = 100'000;
inline constexpr std::size_t kResponseMaxDepth = 32;

DiagnosticResult parse_diagnostic_response(const std::string& text, const DocumentMap& docs, const std::vector<std::string>& kinds);

} // namespace di::design_diagnostic
