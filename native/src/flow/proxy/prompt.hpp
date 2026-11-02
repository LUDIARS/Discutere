#pragma once
// src/flow/proxy/prompt.ts の移植: 代理議論で定立・応酬の prompt に足す部分 (S3)。
// 代理でなければ何も足さない (通常議論の prompt はバイト等価のまま)。
#include <string>
#include <vector>

#include "flow/personas.hpp"

namespace di::flow::proxy {

/** 束と申告の決まり。代理でなければ空配列。 */
std::vector<std::string> proxy_prompt_lines(const FlowPersona& persona);
/** JSON の論拠 1 件に足す項目の書式 (代理のときだけ)。 */
std::string proxy_ground_fields(const FlowPersona& persona);

} // namespace di::flow::proxy
