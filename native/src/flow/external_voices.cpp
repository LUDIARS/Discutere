#include "flow/external_voices.hpp"

#include "discatier_engine_adapter/index.hpp"

namespace di::flow {

ListExternalVoicesFn make_list_external_voices(OpenCoreFn open_core, std::string workspace_id) {
    return [open_core = std::move(open_core), workspace_id = std::move(workspace_id)](const std::vector<std::string>& terms,
                                                                                         int limit) -> std::vector<ContextVoice> {
        // open_core の失敗は TS と同じく呼び出し側へ投げる (try の外)。
        std::unique_ptr<core::Core> core = open_core();
        if (!core) return {};
        std::vector<ContextVoice> out;
        try {
            auto provider = discatier_engine_adapter::create_discatier_context_provider(*core);
            for (const auto& v : provider->list_relevant_external_voices(workspace_id, terms, limit))
                out.push_back({v.content, v.source, v.sourceUrl});
        } catch (...) {
            out.clear();
        }
        try {
            core->close();
        } catch (...) {
        }
        return out;
    };
}

} // namespace di::flow
