#pragma once
// src/flow/proxy/bundle.ts の移植: 人物パック → 代理の prompt に載せる「束」(統合レビュー §2.2、S2)。
//
// - 数値ベクトルは載せない。20D は望む / 不満の次元名を日本語にした説明文にだけ使う
// - 本人の発言は [E1] のように id 付きで載せ、代理はこの id でしか本人の根拠を引けない
// - 予算 (トークン概算) を超えたら、議題と関係の薄い発言から落とす。落とした発言だけを根拠にした
//   立場カードも一緒に落とす。発言が 1 件も載らない予算なら束を作らない (nullopt)
// - confidence=low のパックは束にしない
// 純関数 (DB / LLM なし)。
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json.hpp"
#include "flow/proxy/contract.hpp"

namespace di::flow::proxy {

/**
 * 概算トークン数。ASCII は 1 文字 ≈ 0.3 トークン、非 ASCII は 1 文字 ≈ 1.3 トークン (UTF-16 のコード単位で数える)。
 */
double estimate_tokens(std::string_view text);

std::string dim_label(const std::string& dim);

struct ProxyBundle {
    std::string text;
    std::vector<std::string> evidenceIds;
    /** 根拠 id → 原文 (挿入順)。 */
    std::vector<std::pair<std::string, std::string>> evidenceTexts;
    double estimatedTokens = 0;
    double droppedEvidence = 0;
};

Json to_json(const ProxyBundle& b);

struct BundleOptions {
    double budgetTokens = 0;
    std::vector<std::string> focusTerms;
    std::string label;
};

/** 議題に近い順。同点は正負を交互に、最後は元の順。 */
std::vector<PackEvidence> order_evidence(const std::vector<PackEvidence>& evidence, const std::vector<std::string>& terms);

/** パックを予算内の束にする。束にできなければ nullopt。 */
std::optional<ProxyBundle> render_proxy_bundle(const PersonaPack& pack, const BundleOptions& options);

} // namespace di::flow::proxy
