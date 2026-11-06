#include "flow/discussion_redo.hpp"

#include "base/strings.hpp"

namespace di::flow {

bool is_redo_text(std::string_view text) {
    static const char* const kPrefixes[] = {"再開", "やり直し", "やりなおし", "再議論", "もう一度", "もういちど"};
    const std::string t = str::trim(text);
    for (const char* p : kPrefixes)
        if (str::starts_with(t, p)) return true;
    return false;
}

RedoDecision decide_redo(const RedoInput& input) {
    if (!input.paper) return {false, "no-paper"};
    if (input.paper->status != "started") return {false, "not-started"};
    // やり直せる議論タイプ (完走型でペーパーを使うもの)
    if (input.paper->flow != "discussion" && input.paper->flow != "improvement") return {false, "unsupported-flow"};
    if (input.concluded) return {false, "concluded"};
    if (input.running) return {false, "running"};
    return {true, ""};
}

} // namespace di::flow
