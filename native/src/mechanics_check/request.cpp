#include "mechanics_check/request.hpp"

#include "design_diagnostic/validation.hpp"

namespace di::mechanics_check {

namespace dd = design_diagnostic;

MechanicsCheckRequest validate_mechanics_check_request(const Json& value) {
    if (value.is_object() && value.contains("playModel")) throw MechanicsCheckError("use_level_check", "playModel belongs to /level-check");
    dd::object(value, "request", {"specText", "baselineText", "references"}, "invalid_input");
    auto docs = dd::validate_documents(dd::field(value, "specText"), dd::field(value, "baselineText"), dd::field(value, "references"), "specText");
    MechanicsCheckRequest r;
    r.specText = std::move(docs.primaryText);
    r.baselineText = std::move(docs.baselineText);
    r.references = std::move(docs.references);
    return r;
}

dd::DocumentMap supplied_documents(const MechanicsCheckRequest& request) {
    return dd::supplied_document_map("spec", request.specText, request.baselineText,
                                     request.references.value_or(std::vector<MechanicsReference>{}));
}

} // namespace di::mechanics_check
