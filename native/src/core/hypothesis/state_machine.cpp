#include "core/hypothesis/state_machine.hpp"

namespace di::core::hypothesis {
namespace {
TransitionResult ok(std::string to) { return {true, std::move(to), {}}; }
TransitionResult fail(std::string error) { return {false, {}, std::move(error)}; }
} // namespace

TransitionResult can_transition(std::string_view from, std::string_view action) {
    const bool terminal = from == "integrated" || from == "rejected";
    if (terminal && action != "reject") return fail("cannot transition from terminal state: " + std::string(from));

    if (action == "submit") return from == "draft" ? ok("proposed") : fail("submit requires draft");
    if (action == "validate_theory")
        return from == "proposed" ? ok("under_theory_validation") : fail("validate theory requires proposed");
    if (action == "validate_emotion") {
        // 議論モードでは theory → emotion 厳密順を強制せず、proposed から直接 emotion 検証へ進めるを許容。
        if (from == "proposed" || from == "validated_by_theory") return ok("under_emotion_validation");
        return fail("validate emotion requires proposed or validated_by_theory");
    }
    if (action == "session_end") {
        if (from == "under_theory_validation") return ok("validated_by_theory");
        if (from == "under_emotion_validation") return ok("validated_by_emotion");
        return fail("session end transition is invalid for current state");
    }
    if (action == "integrate") return from == "validated_by_emotion" ? ok("integrated") : fail("integrate requires validated_by_emotion");
    if (action == "reject") return ok("rejected");
    return fail("unknown transition");
}

} // namespace di::core::hypothesis
