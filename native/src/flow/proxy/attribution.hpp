#pragma once
// src/flow/proxy/attribution.ts の移植: 代理の主張の帰属検査 (統合レビュー §1, S3)。
// 論拠 1 件ごとに本人の発言との関係を 4 区分 (sourceQuote / groundedParaphrase / simulationInference / unknown)
// で申告させ、コードで検査する。決定的 (LLM なし)。検査ができない状態を ok にしない。
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"

namespace di::flow::proxy {

/** "sourceQuote" | "groundedParaphrase" | "simulationInference" | "unknown" */
using ClaimKind = std::string;

const std::vector<ClaimKind>& claim_kinds();
/** 本人の発言として扱う区分 (sourceQuote / groundedParaphrase)。 */
bool is_grounded_kind(const ClaimKind& kind);
std::optional<ClaimKind> coerce_claim_kind(const Json& v);
/** 文字列の配列から E 番号 (E1〜E999) を重複なく取り出す ([E2] の括弧も外す)。 */
std::vector<std::string> coerce_evidence_ids(const Json& v);

/** 代理が引いてよい根拠 (束に載った発言)。 */
struct AttributionScope {
    std::vector<std::string> evidenceIds;
    std::map<std::string, std::string> evidenceTexts;
};

/** "unsupported_attribution" | "quote_mismatch" */
using AttributionFoul = std::string;

struct AttributionResult {
    bool ok = false;
    std::optional<AttributionFoul> foul;
    ClaimKind claimKind;
    std::vector<std::string> evidenceIds;
};

/** 引用部 (「」や "" で囲んだ 4 文字以上、無ければ data 全体) が引いた発言のどれかに含まれるか。 */
bool quote_appears_in_evidence(std::string_view data, const std::vector<std::string>& cited);

struct CheckAttributionArgs {
    std::optional<ClaimKind> claimKind;
    std::vector<std::string> evidenceIds;
    std::string data;
    AttributionScope scope;
};

/** 1 件の主張を検査する。claimKind 未申告は推測として扱う。 */
AttributionResult check_attribution(const CheckAttributionArgs& args);

/** 露出文に付ける帰属の印 (本人の発言か推測か)。 */
std::string attribution_suffix(const ClaimKind& kind, const std::vector<std::string>& evidence_ids);

Json to_json(const AttributionResult& r);

} // namespace di::flow::proxy
