#pragma once
// src/flow/user-voices/mix.ts の移植: ユーザーの声の均等混合 (spec/feature/flow/user-voices.md)。
// 議題のゲームと類似ゲームの声を、ゲームごとに同じ件数ずつ交互に取り出して混ぜる。同じ本文は 1 回だけ使う。
#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "flow/discussion_paper.hpp"
#include "flow/voice_cache.hpp"

namespace di::flow::user_voices {

/** グループを先頭から 1 件ずつ交互に取り、limit 件まで混ぜる (純関数)。 */
template <class T, class KeyOf>
std::vector<T> mix_evenly(const std::vector<std::vector<T>>& groups, long limit, KeyOf key_of) {
    std::vector<T> out;
    std::set<std::string> seen;
    std::vector<std::size_t> cursors(groups.size(), 0);
    bool progressed = true;
    while (static_cast<long>(out.size()) < limit && progressed) {
        progressed = false;
        for (std::size_t g = 0; g < groups.size() && static_cast<long>(out.size()) < limit; ++g) {
            const auto& group = groups[g];
            while (cursors[g] < group.size()) {
                const T& item = group[cursors[g]];
                cursors[g] += 1;
                if (!seen.insert(key_of(item)).second) continue;
                out.push_back(item);
                progressed = true;
                break;
            }
        }
    }
    return out;
}

/** 声 1 件の重複判定キー (前後空白を除いた本文)。 */
std::string voice_key(const ContextVoice& v);

using FetchVoiceGroups = std::function<std::vector<std::vector<ContextVoice>>(const std::vector<std::string>& terms,
                                                                              int per_group_limit)>;

/** 議論フローに渡す lookup を、ゲームごとの声の均等混合にする (fetchGroups の先頭が議題のゲーム)。 */
ListExternalVoicesFn make_mixed_voice_lookup(FetchVoiceGroups fetch_groups);

} // namespace di::flow::user_voices
