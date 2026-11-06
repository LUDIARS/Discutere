#pragma once
// src/persona-engine/llm/pricing.ts — モデル別トークン単価から「等価 API コスト (USD)」を推定する。
// cost_usd は実課金ではなく等価 API 換算の推定値。cache は 5 分 ephemeral 前提 (write=入力×1.25 / read=入力×0.1)。
#include <optional>
#include <string>

#include "persona_engine/llm/client.hpp"

namespace di::persona_engine {

/** model id (例 `claude-opus-4-8`、`@effort` 付き可) をファミリ ("opus" / "sonnet" / "haiku") に正規化。未知は nullopt。 */
std::optional<std::string> model_family(const std::optional<std::string>& model);

/** usage の token と model から等価 API コスト (USD) を推定する。単価表に無いモデル・token 皆無なら nullopt。 */
std::optional<double> estimate_cost_usd(const std::optional<std::string>& model, const std::optional<TokenUsage>& usage);

} // namespace di::persona_engine
