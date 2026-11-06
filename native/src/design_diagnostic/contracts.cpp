#include "design_diagnostic/contracts.hpp"

namespace di::design_diagnostic {

Json to_json(const DiagnosticReference& v) {
    Json o = Json::object();
    o["id"] = v.id;
    o["source"] = v.source;
    if (v.revision) o["revision"] = *v.revision;
    o["text"] = v.text;
    o["evidenceKind"] = v.evidenceKind;
    if (v.domain) o["domain"] = *v.domain;
    return o;
}

Json to_json(const EvidenceCitation& v) { return Json{{"referenceId", v.referenceId}, {"quote", v.quote}}; }

Json to_json(const ComparedStatement& v) {
    Json evidence = Json::array();
    for (const auto& e : v.evidence) evidence.push_back(to_json(e));
    return Json{{"text", v.text}, {"evidence", evidence}};
}

namespace {
void fill(Json& o, const DiagnosticFinding& v) {
    o["target"] = v.target;
    o["left"] = to_json(v.left);
    o["right"] = to_json(v.right);
    o["status"] = v.status;
    o["causeDomain"] = v.causeDomain;
    o["conditions"] = v.conditions;
    o["unknowns"] = v.unknowns;
}
} // namespace

Json to_json(const DiagnosticFinding& v) {
    Json o = Json::object();
    fill(o, v);
    return o;
}

Json to_json(const KindedFinding& v) {
    Json o = Json::object();
    o["kind"] = v.kind;
    fill(o, v);
    return o;
}

Json to_json(const DiagnosticResult& v) {
    Json logic = Json::array();
    for (const auto& f : v.logic_differences) logic.push_back(to_json(f));
    Json gaps = Json::array();
    for (const auto& g : v.design_gap) gaps.push_back(to_json(g));
    return Json{{"logic_differences", logic}, {"design_gap", gaps}};
}

} // namespace di::design_diagnostic
