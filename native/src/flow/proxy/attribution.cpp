#include "flow/proxy/attribution.hpp"

#include <algorithm>
#include <set>

#include "base/strings.hpp"
#include "flow/text_scan.hpp"

namespace di::flow::proxy {

const std::vector<ClaimKind>& claim_kinds() {
    static const std::vector<ClaimKind> kinds = {"sourceQuote", "groundedParaphrase", "simulationInference", "unknown"};
    return kinds;
}

bool is_grounded_kind(const ClaimKind& kind) { return kind == "sourceQuote" || kind == "groundedParaphrase"; }

std::optional<ClaimKind> coerce_claim_kind(const Json& v) {
    if (!v.is_string()) return std::nullopt;
    const auto s = v.get<std::string>();
    const auto& kinds = claim_kinds();
    if (std::find(kinds.begin(), kinds.end(), s) == kinds.end()) return std::nullopt;
    return s;
}

std::vector<std::string> coerce_evidence_ids(const Json& v) {
    std::vector<std::string> out;
    if (!v.is_array()) return out;
    for (const auto& item : v) {
        if (!item.is_string()) continue;
        // item.trim().replace(/^\[|\]$/g, "")
        std::string id = str::trim(item.get<std::string>());
        if (!id.empty() && id.front() == '[') id.erase(id.begin());
        if (!id.empty() && id.back() == ']') id.pop_back();
        if (text_scan::is_evidence_id(id) && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return out;
}

bool quote_appears_in_evidence(std::string_view data, const std::vector<std::string>& cited) {
    std::vector<std::string> targets;
    for (const auto& seg : text_scan::quoted_segments(data, 4)) targets.push_back(text_scan::remove_js_spaces(seg));
    if (targets.empty()) targets.push_back(text_scan::remove_js_spaces(data));
    std::vector<std::string> sources;
    for (const auto& c : cited) sources.push_back(text_scan::remove_js_spaces(c));
    for (const auto& t : targets) {
        if (str::js_length(t) < 4) continue;
        for (const auto& s : sources)
            if (s.find(t) != std::string::npos) return true;
    }
    return false;
}

AttributionResult check_attribution(const CheckAttributionArgs& args) {
    const ClaimKind claim_kind = args.claimKind.value_or("simulationInference");
    const std::set<std::string> allowed(args.scope.evidenceIds.begin(), args.scope.evidenceIds.end());
    std::vector<std::string> cited;
    for (const auto& id : args.evidenceIds)
        if (allowed.count(id)) cited.push_back(id);
    if (!is_grounded_kind(claim_kind)) return {true, std::nullopt, claim_kind, cited};
    if (args.evidenceIds.empty() || cited.size() != args.evidenceIds.size())
        return {false, std::string("unsupported_attribution"), claim_kind, cited};
    if (claim_kind == "sourceQuote") {
        std::vector<std::string> texts;
        for (const auto& id : cited) {
            auto it = args.scope.evidenceTexts.find(id);
            texts.push_back(it == args.scope.evidenceTexts.end() ? std::string() : it->second);
        }
        if (!quote_appears_in_evidence(args.data, texts)) return {false, std::string("quote_mismatch"), claim_kind, cited};
    }
    return {true, std::nullopt, claim_kind, cited};
}

std::string attribution_suffix(const ClaimKind& kind, const std::vector<std::string>& evidence_ids) {
    if (is_grounded_kind(kind)) return evidence_ids.empty() ? "" : " [" + str::join(evidence_ids, ",") + "]";
    return kind == "unknown" ? "（材料不足）" : "（推測）";
}

Json to_json(const AttributionResult& r) {
    Json o = Json::object();
    o["ok"] = r.ok;
    if (r.foul) o["foul"] = *r.foul;
    o["claimKind"] = r.claimKind;
    o["evidenceIds"] = r.evidenceIds;
    return o;
}

} // namespace di::flow::proxy
