#pragma once
// トークン数の概算 (tokens.ts)。正確なトークナイザは持たない。文字種別の経験則で見積もる。
// - CJK 文字は概ね 1.5 文字 / token、それ以外は概ね 4 文字 / token。
#include <string_view>

namespace di::lib::llm_gateway {

/** 文字列のトークン数を概算する (code point 単位で数える)。 */
long long estimate_tokens(std::string_view text);

} // namespace di::lib::llm_gateway
