#include "flow/user_voices/mix.hpp"

#include "base/strings.hpp"

namespace di::flow::user_voices {

std::string voice_key(const ContextVoice& v) { return str::trim(v.content); }

ListExternalVoicesFn make_mixed_voice_lookup(FetchVoiceGroups fetch_groups) {
    return [fetch_groups = std::move(fetch_groups)](const std::vector<std::string>& terms, int limit) {
        if (limit <= 0) return std::vector<ContextVoice>{};
        // 均等に取るために、各ゲームから limit 件ずつ候補を引いてから交互に混ぜる。
        const auto groups = fetch_groups(terms, limit);
        return mix_evenly(groups, limit, voice_key);
    };
}

} // namespace di::flow::user_voices
