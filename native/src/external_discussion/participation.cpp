// @implements DI-EXTERNAL-DISCUSSION
#include "external_discussion/participation.hpp"

#include <algorithm>

#include "base/crypto.hpp"
#include "base/strings.hpp"

namespace di::external_discussion {

std::optional<Opportunity> opportunity(const ConversationInput& input, std::int64_t now) {
    if (!input.enabled) return std::nullopt;
    std::vector<const ConversationMessage*> humans;
    for (const auto& m : input.messages)
        if (m.kind == "human" && !str::trim(m.text).empty()) humans.push_back(&m);
    if (humans.empty()) return std::nullopt;
    const ConversationMessage& last = *humans.back();
    if (last.at > now || now - last.at > 86'400'000) return std::nullopt;
    // 最後の人間の発言の後に bot が応答していれば、この機会は終わっている。
    for (const auto& m : input.messages)
        if (m.kind == "ai" && m.at >= last.at) return std::nullopt;
    const bool quiet = now - last.at >= SILENCE_MS;
    if (!quiet) {
        const auto recent = std::count_if(humans.begin(), humans.end(), [&](const ConversationMessage* m) { return m->at >= now - SILENCE_MS; });
        if (recent < 3) return std::nullopt;
    }
    const std::string mode = quiet ? "quiet" : "active";
    const std::string id = crypto::sha256_hex(json_stringify(Json::array({input.scene, last.id, last.text, mode})));
    return Opportunity{id, last.id, last.at, mode};
}

} // namespace di::external_discussion
