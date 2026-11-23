#include "flow/dialectic/toulmin.hpp"

#include <vector>

#include "base/strings.hpp"

namespace di::flow::dialectic {

namespace {
bool has(const std::optional<std::string>& s) { return s && !str::trim(*s).empty(); }
} // namespace

std::string render_qualifier(const PositionRecord& p) {
    std::vector<std::string> parts;
    if (has(p.qualifier)) parts.push_back("範囲: " + *p.qualifier);
    if (has(p.qualifierForce)) parts.push_back("確度: " + *p.qualifierForce);
    return parts.empty() ? std::string("(明示なし)") : str::join(parts, " / ");
}

std::string render_exceptions(const PositionRecord& p) {
    return p.exceptions.empty() ? std::string() : "例外: " + str::join(p.exceptions, " / ");
}

std::string render_warrant_lines(const Ground& g, const std::string& indent) {
    std::string out;
    if (has(g.warrant)) out += "\n" + indent + "論拠: " + *g.warrant;
    if (has(g.backing)) out += "\n" + indent + "裏付け: " + *g.backing;
    return out;
}

} // namespace di::flow::dialectic
