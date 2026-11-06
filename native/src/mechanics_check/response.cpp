#include "mechanics_check/response.hpp"

#include "design_diagnostic/response.hpp"
#include "mechanics_check/request.hpp"

namespace di::mechanics_check {

MechanicsCheckResult parse_mechanics_check_response(const std::string& text, const MechanicsCheckRequest& request) {
    return design_diagnostic::parse_diagnostic_response(text, supplied_documents(request), mechanics_difference_kinds());
}

} // namespace di::mechanics_check
