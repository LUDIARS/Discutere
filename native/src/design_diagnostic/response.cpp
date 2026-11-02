#include "design_diagnostic/response.hpp"

#include <algorithm>
#include <set>

#include "base/strings.hpp"
#include "design_diagnostic/validation.hpp"

namespace di::design_diagnostic {

namespace {

const std::string C = "invalid_response";

const Json& at(const Json& object, const char* key) {
    static const Json kMissing;
    const Json* f = field(object, key);
    return f ? *f : kMissing;
}

std::vector<std::string> strings(const Json& value, const std::string& path) {
    const auto& L = DIAGNOSTIC_LIMITS;
    array(value, path, L.conditions, C);
    const Json& arr = value;
    std::vector<std::string> out;
    for (std::size_t i = 0; i < arr.size(); ++i) out.push_back(string(arr[i], path + "[" + std::to_string(i) + "]", L.shortTextChars, C));
    return out;
}

ComparedStatement statement(const Json& value, const std::string& path, const DocumentMap& docs) {
    const auto& L = DIAGNOSTIC_LIMITS;
    object(value, path, {"text", "evidence"}, C);
    const Json& s = value;
    const Json& arr = at(s, "evidence");
    array(arr, path + ".evidence", L.references, C);
    ComparedStatement out;
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const std::string p = path + ".evidence[" + std::to_string(i) + "]";
        object(arr[i], p, {"referenceId", "quote"}, C);
        const Json& cite = arr[i];
        const std::string reference_id = id(at(cite, "referenceId"), p + ".referenceId", C);
        const std::string quote = string(at(cite, "quote"), p + ".quote", L.shortTextChars, C);
        const auto* doc = docs.get(reference_id);
        if (!doc || doc->text.find(quote) == std::string::npos) fail(C, p, "citation is not an exact extract of a supplied document");
        out.evidence.push_back({reference_id, quote});
    }
    out.text = string(at(s, "text"), path + ".text", L.statementChars, C);
    return out;
}

struct Parsed {
    const Json* raw;
    DiagnosticFinding finding;
};

bool grounded(const ComparedStatement& s, const DocumentMap& docs) {
    return std::any_of(s.evidence.begin(), s.evidence.end(), [&](const EvidenceCitation& e) {
        const auto* doc = docs.get(e.referenceId);
        return !doc || doc->evidenceKind != "causal_hypothesis";
    });
}

Parsed finding(const Json& value, const std::string& path, const DocumentMap& docs) {
    const auto& L = DIAGNOSTIC_LIMITS;
    object(value, path, {"kind", "target", "left", "right", "status", "causeDomain", "conditions", "unknowns"}, C);
    const Json& raw = value;
    ComparedStatement left = statement(at(raw, "left"), path + ".left", docs);
    ComparedStatement right = statement(at(raw, "right"), path + ".right", docs);
    std::string status = choice(at(raw, "status"), consistency_statuses(), path + ".status", C);
    std::vector<std::string> conditions = strings(at(raw, "conditions"), path + ".conditions");
    std::vector<std::string> unknowns = strings(at(raw, "unknowns"), path + ".unknowns");
    std::string cause_domain = choice(at(raw, "causeDomain"), cause_domains(), path + ".causeDomain", C);
    const bool g_left = grounded(left, docs);
    const bool g_right = grounded(right, docs);
    if (status != "unknown" && (!g_left || !g_right)) {
        status = "unknown";
        if (unknowns.size() >= L.conditions) fail(C, path, "no room to describe unsupported conclusion");
        unknowns.push_back("比較の片側に入力資料の根拠がない、または原因仮説だけなので判定は unknown。");
    }
    std::set<std::string> cited_domains;
    for (const auto* side : {&left, &right}) {
        for (const auto& e : side->evidence) {
            const auto* doc = docs.get(e.referenceId);
            if (doc && doc->domain && doc->evidenceKind != "causal_hypothesis") cited_domains.insert(*doc->domain);
        }
    }
    const std::vector<std::string> required = cause_domain == "both" ? std::vector<std::string>{"mechanics", "level"}
                                                                     : std::vector<std::string>{cause_domain};
    const bool missing_domain = std::any_of(required.begin(), required.end(), [&](const std::string& d) { return !cited_domains.count(d); });
    if (cause_domain != "unknown" && (conditions.empty() || !g_left || !g_right || missing_domain)) {
        cause_domain = "unknown";
        if (unknowns.size() >= L.conditions) fail(C, path, "no room to describe unsupported cause classification");
        unknowns.push_back("原因の領域を分類する比較条件または領域別の資料根拠がないため causeDomain は unknown。");
    }
    if (status != "unknown" && conditions.empty()) fail(C, path, "known conclusion needs explicit conditions");
    if (status == "unknown" && unknowns.empty()) fail(C, path, "unknown conclusion needs missing evidence or conditions");
    DiagnosticFinding f;
    f.target = string(at(raw, "target"), path + ".target", L.shortTextChars, C);
    f.left = std::move(left);
    f.right = std::move(right);
    f.status = std::move(status);
    f.causeDomain = std::move(cause_domain);
    f.conditions = std::move(conditions);
    f.unknowns = std::move(unknowns);
    return {&raw, std::move(f)};
}

KindedFinding with_kind(const DiagnosticFinding& f, std::string kind, std::optional<std::string> status = std::nullopt) {
    KindedFinding k;
    static_cast<DiagnosticFinding&>(k) = f;
    k.kind = std::move(kind);
    if (status) k.status = *status;
    return k;
}

} // namespace

DiagnosticResult parse_diagnostic_response(const std::string& text, const DocumentMap& docs, const std::vector<std::string>& kinds) {
    const auto& L = DIAGNOSTIC_LIMITS;
    if (text.size() > L.responseBytes) fail(C, "response", "response exceeds byte limit");
    auto parsed = parse_json(text, JsonLimits{L.responseBytes, kResponseMaxElements, kResponseMaxDepth});
    if (!parsed) fail(C, "response", "expected JSON only");
    object(*parsed, "response", {"logic_differences", "design_gap"}, C);
    const Json& result = *parsed;

    DiagnosticResult out;
    const Json& logic = at(result, "logic_differences");
    array(logic, "logic_differences", L.findingsPerKind, C);
    for (std::size_t i = 0; i < logic.size(); ++i) {
        const std::string path = "logic_differences[" + std::to_string(i) + "]";
        auto f = finding(logic[i], path, docs);
        out.logic_differences.push_back(with_kind(f.finding, choice(at(*f.raw, "kind"), kinds, path + ".kind", C)));
    }
    const Json& gaps = at(result, "design_gap");
    array(gaps, "design_gap", L.findingsPerKind, C);
    for (std::size_t i = 0; i < gaps.size(); ++i) {
        const std::string path = "design_gap[" + std::to_string(i) + "]";
        auto f = finding(gaps[i], path, docs);
        const std::string kind = choice(at(*f.raw, "kind"), {"missing_evidence", "missing_condition", "contradiction"}, path + ".kind", C);
        if (kind == "contradiction" && f.finding.status == "inconsistent") {
            out.design_gap.push_back(with_kind(f.finding, kind, "inconsistent"));
            continue;
        }
        if (f.finding.status != "unknown") fail(C, path, "missing evidence/condition gaps must be unknown; contradictions must be inconsistent");
        out.design_gap.push_back(with_kind(f.finding, kind == "contradiction" ? "missing_evidence" : kind, "unknown"));
    }
    if (out.logic_differences.empty() && out.design_gap.empty()) fail(C, "response", "empty diagnostic is not a successful result");
    // A downgraded unsupported comparison must also expose the missing evidence as a gap.
    for (const auto& difference : out.logic_differences) {
        const bool downgraded = difference.status == "unknown" ||
                                std::any_of(difference.unknowns.begin(), difference.unknowns.end(),
                                            [](const std::string& u) { return str::contains(u, "causeDomain は unknown"); });
        const auto same_target = [&](const DesignGap& g) { return g.target == difference.target; };
        if (downgraded && std::none_of(out.design_gap.begin(), out.design_gap.end(), same_target))
            out.design_gap.push_back(with_kind(difference, "missing_evidence", "unknown"));
        if (difference.status == "inconsistent" && difference.kind != "revision" &&
            std::none_of(out.design_gap.begin(), out.design_gap.end(),
                         [&](const DesignGap& g) { return g.target == difference.target && g.status == "inconsistent"; }))
            out.design_gap.push_back(with_kind(difference, "contradiction", "inconsistent"));
    }
    return out;
}

} // namespace di::design_diagnostic
