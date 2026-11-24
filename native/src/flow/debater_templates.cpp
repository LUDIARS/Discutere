#include "flow/debater_templates.hpp"

#include <cstdint>
#include <set>
#include <stdexcept>

#include "base/fsutil.hpp"
#include "base/log.hpp"
#include "base/sidecar.hpp"
#include "playtest/contracts/runtime.hpp" /* augur-inject:import:760fb77e */
#include "contracts/debater_templates.hpp" /* augur-inject:contract-predicate:602250b5 */

namespace di::flow {
namespace {

std::vector<std::string> load_template_ids() {
        const auto path = fs::join(sidecar::repo_root(), "data/debater-templates.json");
        const auto text = fs::read_file(path, 1u << 16);
        if (!text) throw std::runtime_error("議論者テンプレートを読めません: " + path);
        const auto catalog = parse_json_or_throw(*text);
        const auto entries = json_array(catalog, "templates");
        if (json_string(catalog, "version").empty() || entries.empty())
            throw std::runtime_error("議論者テンプレートの版または一覧がありません");
        std::vector<std::string> result;
        std::set<std::string> unique;
        for (const auto& entry : entries) {
            auto id = json_string(entry, "id");
            if (!id.starts_with("tpl-") || id.size() <= 4 || id.size() > 128 ||
                id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") != std::string::npos ||
                !unique.insert(id).second)
                throw std::runtime_error("議論者テンプレート ID が不正または重複しています");
            result.push_back(std::move(id));
        }
        return result;
}

// テンプレートが読めなくても議論は止めない。割り当てを省き、Di の論述パターンだけで進める。
const std::vector<std::string>& template_ids() {
    static const auto ids = [] {
        try {
            return load_template_ids();
        } catch (const std::exception& e) {
            log::warn(std::string("議論者テンプレートを使わずに続けます: ") + e.what());
            return std::vector<std::string>{};
        }
    }();
    return ids;
}

std::uint32_t template_seed(const std::string& session_id, const std::string& pool_persona_id) {
    // FNV-1a over bytes keeps the assignment independent of std::hash and platform.
    std::uint32_t hash = 2166136261u;
    for (unsigned char byte : session_id + '\n' + pool_persona_id) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

} // namespace

void assign_debater_templates(std::vector<FlowPersona>& cast, const std::string& session_id) { return ::augur::invoke_observed(::augur::ContractSpec{"C-13", "0544d933", "src/flow/debater_templates.cpp:51"}, di::flow::checks::debater_templates, [&]() -> void { /* augur-inject:contract-wrap:0544d933 */
    std::set<std::size_t> used;
    for (auto& persona : cast) {
        if (persona.role == "facilitator" || persona.proxy || !persona.poolPersonaId) continue;
        const auto& ids = template_ids();
        if (ids.empty()) return;
        if (used.size() == ids.size()) used.clear();
        auto index = template_seed(session_id, *persona.poolPersonaId) % ids.size();
        while (!used.insert(index).second) index = (index + 1) % ids.size();
        persona.debaterTemplateId = ids[index];
    }
}, cast, session_id); /* augur-inject:contract-end:0544d933 */ }

} // namespace di::flow
