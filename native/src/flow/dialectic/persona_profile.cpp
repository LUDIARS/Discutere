#include "flow/dialectic/persona_profile.hpp"

#include "base/strings.hpp"

namespace di::flow::dialectic {

std::string persona_profile_line(const FlowPersona& persona) {
    if (persona.proxy) return "";
    std::string profile;
    if (persona.playerProfile && !str::trim(*persona.playerProfile).empty()) profile = str::trim(*persona.playerProfile);
    else if (persona.possession && persona.possession->descriptor && !str::trim(*persona.possession->descriptor).empty())
        profile = str::trim(*persona.possession->descriptor);
    if (profile.empty()) return "";
    return "あなたの遊び手としての型: " + profile +
           " — この型の遊び手として論点をどう感じるかを、根拠と一緒に述べる (型の名前を名乗らない。立場は変えない)。";
}

} // namespace di::flow::dialectic
