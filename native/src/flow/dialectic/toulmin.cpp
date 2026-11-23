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

std::string render_position_compact(const PositionRecord& p, const std::string& label, const std::optional<std::string>& focus_ground_id) {
    std::vector<std::string> lines{label + " (" + p.stance + "): " + p.claim, "  限定: " + render_qualifier(p)};
    if (const std::string ex = render_exceptions(p); !ex.empty()) lines.push_back("  " + ex);
    for (const auto& g : p.grounds) {
        if (focus_ground_id && g.id == *focus_ground_id)
            lines.push_back("  - [" + g.id + "] (" + g.state + ") データ: " + g.text + render_warrant_lines(g, "      "));
        else
            lines.push_back("  - [" + g.id + "] (" + g.state + ") " + str::js_slice(str::squish(g.text), 0, 60));
    }
    return str::join(lines, "\n");
}

} // namespace di::flow::dialectic
