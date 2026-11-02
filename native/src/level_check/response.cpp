#include "level_check/response.hpp"

#include "design_diagnostic/response.hpp"
#include "level_check/request.hpp"

namespace di::level_check {

design_diagnostic::DiagnosticResult parse_level_check_response(const std::string& text, const LevelCheckRequest& request) {
    return design_diagnostic::parse_diagnostic_response(text, supplied_level_documents(request), level_difference_kinds());
}

} // namespace di::level_check
