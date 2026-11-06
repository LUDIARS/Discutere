#include "flow/persona_display.hpp"

#include "base/strings.hpp"

namespace di::flow {

std::string stance_label(const FlowStance& stance) {
    if (stance == "pro") return "賛成派";
    if (stance == "con") return "反対派";
    if (stance == "opinion") return "意見屋";
    if (stance == "neutral") return "進行役";
    if (stance == "reform") return "改革派";
    if (stance == "conservative") return "保守派";
    return "";
}

std::string role_label(const FlowRole& role) {
    if (role == "facilitator") return "進行役";
    if (role == "debater") return "論者";
    if (role == "opinion") return "意見屋";
    return "";
}

std::string compose_display_name(const DisplayNameInput& input) {
    // input.stance ? … : input.role ? … : "" (空文字のスタンス/ロールは偽)
    const std::string label = input.stance && !input.stance->empty() ? stance_label(*input.stance)
                              : input.role && !input.role->empty()   ? role_label(*input.role)
                                                                     : std::string();
    const std::string possession = input.possessionName ? str::trim(*input.possessionName) : std::string();
    if (label.empty()) return input.name;
    const std::string inner = possession.empty() ? label : label + "/" + possession;
    return input.name + " (" + inner + ")";
}

} // namespace di::flow
