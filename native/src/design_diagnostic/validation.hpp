#pragma once
// Shared primitives for strict bounded I/O validation, with no coercion or truncation
// (src/design-diagnostic/validation.ts の移植)。失敗は DiagnosticError(code, "<path>: <reason>")。
// 長さは JS と同じ UTF-16 単位で数える。
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"
#include "design_diagnostic/contracts.hpp"

namespace di::design_diagnostic {

[[noreturn]] void fail(const std::string& code, const std::string& path, const std::string& reason);
/** object で、keys 以外のキーを持たないこと。 */
const Json& object(const Json& value, const std::string& path, const std::vector<std::string>& keys, const std::string& code);
/** 空白だけでない文字列で、長さ max 以下。 */
std::string string(const Json& value, const std::string& path, std::size_t max, const std::string& code);
/** 配列で、要素数 min..max。 */
const Json& array(const Json& value, const std::string& path, std::size_t max, const std::string& code, std::size_t min = 0);
/** 有限の数。 */
double finite(const Json& value, const std::string& path, const std::string& code);
/** choices のどれかの文字列。 */
std::string choice(const Json& value, const std::vector<std::string>& choices, const std::string& path, const std::string& code);
/** ID (80 文字以下、/^[A-Za-z0-9][A-Za-z0-9_.:-]*$/)。 */
std::string id(const Json& value, const std::string& path, const std::string& code);
/** 重複が無いこと。 */
void unique(const std::vector<std::string>& values, const std::string& path, const std::string& code);

} // namespace di::design_diagnostic
