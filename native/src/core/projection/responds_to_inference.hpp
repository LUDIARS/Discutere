#pragma once
// 引用行 "> @uid:<id>" から応答先の発話 id を読む (src/core/projection/responds-to-inference.ts)。
#include <optional>
#include <string>
#include <string_view>

namespace di::core::projection {

std::optional<std::string> infer_responds_to(std::string_view raw_content);

} // namespace di::core::projection
