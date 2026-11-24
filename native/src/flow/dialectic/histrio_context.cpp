#include "flow/dialectic/histrio_context.hpp"

#include <algorithm>
#include <future>
#include <map>

#include "base/parallel.hpp"
#include "playtest/contracts/runtime.hpp" /* augur-inject:import:b0c87704 */
#include "contracts/histrio_pattern.hpp" /* augur-inject:contract-predicate:d5efc091 */

namespace di::flow::dialectic {
namespace {

using integrations::HistrioProfile;
using integrations::HistrioScriptPattern;

std::optional<LifeScriptContext> resolve_histrio_pattern(const HistrioProfile& profile,
                                                        const std::optional<std::vector<HistrioScriptPattern>>& remote,
                                                        const ArgumentPatternCatalog& local) { return ::augur::invoke_observed(::augur::ContractSpec{"C-12", "3167948a", "src/flow/dialectic/histrio_context.cpp:15"}, di::flow::dialectic::checks::histrio_pattern, [&]() -> std::optional<LifeScriptContext> { /* augur-inject:contract-wrap:3167948a */
    if (remote) {
        for (const auto& pattern : *remote) {
            if (pattern.id == profile.patternId && pattern.version == profile.patternVersion)
                return LifeScriptContext{pattern.id, pattern.version, "histrio", pattern.label,
                                         pattern.healthy, pattern.healthy, pattern.stress, profile.tone};
        }
        return std::nullopt;
    }
    for (const auto& pattern : local.scriptPatterns) {
        if (pattern.id == profile.patternId)
            return LifeScriptContext{pattern.id, local.version, "di", pattern.label,
                                     pattern.position, pattern.exchange, pattern.stress, profile.tone};
    }
    return std::nullopt;
}, profile, remote, local); /* augur-inject:contract-end:3167948a */ }

bool uses_histrio(const FlowPersona& persona) {
    return persona.poolPersonaId && persona.role != "facilitator" && !persona.proxy;
}

std::string_view loggable_id(std::string_view id) {
    if (id.size() > 128 || std::ranges::any_of(id, [](unsigned char c) { return c < 32 || c > 126; }))
        return "不正な人物 ID";
    return id;
}

} // namespace

void apply_histrio_context(std::vector<FlowPersona>& cast, const integrations::HistrioClient& client,
                           const ArgumentPatternCatalog& local, const WarnFn& warn) {
    if (!client.enabled()) return;
    std::map<std::string, std::vector<std::size_t>> by_id;
    for (std::size_t i = 0; i < cast.size(); ++i)
        if (uses_histrio(cast[i])) by_id[*cast[i].poolPersonaId].push_back(i);
    if (by_id.empty()) return;

    std::vector<std::string> ids;
    for (const auto& entry : by_id) ids.push_back(entry.first);
    const auto remote = [&] {
        auto catalog = std::async(std::launch::async, [&client] { return client.script_patterns(); });
        auto profiles = parallel_map(ids, [&client](const std::string& id) { return client.profile(id); });
        return std::pair{catalog.get(), std::move(profiles)};
    }();

    if (!remote.first.value && warn)
        warn("Histrio 脚本カタログ: " + remote.first.error + "。取得できたプロフィールには Di の写しを使います。");
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto& profile = remote.second[i];
        const auto script = profile.value ? resolve_histrio_pattern(*profile.value, remote.first.value, local) : std::nullopt;
        if (!script && warn)
            warn("Histrio プロフィール (" + std::string(loggable_id(ids[i])) + "): " +
                 (profile.value ? "対応する脚本がありません" : profile.error) + "。この人物の脚本行を省きます。");
        for (const auto index : by_id.at(ids[i])) {
            auto& persona = cast[index];
            persona.scriptPattern = script ? std::optional(script->id) : std::nullopt;
            persona.lifeScript = script;
        }
    }
}

} // namespace di::flow::dialectic
