#pragma once
// src/flow/anatomia/refine.ts の移植: Anatomia ドメインカード → Di メカニクス (MechanicSummary) の精製。
// 1. infra / presentation / transport などプレイヤーに見えない層を除外する。
// 2. description を player-facing に書き直し、intended_affect (狙う体験) を補う。
// LLM が無い / 失敗したときは決定論フォールバック (実装メカニクススラグを持つドメインだけ採用) に degrade し warn に出す。
// DB / プロセスに依存しない純ロジック。LLM 応答は上限つきで読む。
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "flow/anatomia/types.hpp"
#include "flow/investigate.hpp"
#include "persona_engine/llm/client.hpp"

namespace di::flow::anatomia {

struct RefineDomainsDeps {
    persona_engine::LLMClient* llm = nullptr;
    std::optional<std::string> theme;
    std::optional<std::string> model;
    std::function<void(const std::string&)> warn;
};

/** LLM 応答の上限 (JSON 配列部分)。 */
inline constexpr std::size_t kRefineResponseMaxBytes = 1u << 20;

std::vector<MechanicSummary> fallback_domains_to_mechanics(const std::vector<AnatomiaDomainCard>& cards);

/** 精製の prompt (テスト用に公開)。 */
std::string build_refine_prompt(const std::vector<AnatomiaDomainCard>& cards, const std::optional<std::string>& theme);

/** LLM 応答から最初の '[' と最後の ']' の間を JSON 配列として読み、MechanicSummary に正規化する。読めなければ nullopt。 */
std::optional<std::vector<MechanicSummary>> parse_refine_response(const std::string& raw);

std::vector<MechanicSummary> refine_domains_to_mechanics(const std::vector<AnatomiaDomainCard>& cards, const RefineDomainsDeps& deps = {});

} // namespace di::flow::anatomia
