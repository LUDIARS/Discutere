#pragma once
// src/flow/voice-cache.ts の移植: 発話時の外部の声の参照キャッシュ (OVERVIEW §6, item5)。
// 1 議論セッションのあいだ lookup の結果をメモ化し、全ペルソナで使い回す。キー = 正規化した検索語 + limit。
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "flow/discussion_paper.hpp"

namespace di::flow {

using ListExternalVoicesFn = std::function<std::vector<ContextVoice>(const std::vector<std::string>& terms, int limit)>;

class VoiceCache {
public:
    /** fn 未指定なら常に空配列を返すスタブ。 */
    explicit VoiceCache(ListExternalVoicesFn fn = {}) : fn_(std::move(fn)) {}
    /** キャッシュ経由で外部の声を引く (初回のみ実 lookup)。 */
    std::vector<ContextVoice> lookup(const std::vector<std::string>& terms, int limit);
    /** lookup を関数として渡す用。キャッシュ本体より長く使わないこと。 */
    ListExternalVoicesFn as_fn();
    int misses() const;
    int hits() const;

private:
    ListExternalVoicesFn fn_;
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<ContextVoice>> cache_;
    int hits_ = 0;
    int misses_ = 0;
};

/** createVoiceCache(fn)。 */
std::shared_ptr<VoiceCache> create_voice_cache(ListExternalVoicesFn fn = {});

/** キャッシュキー (テスト用に公開)。JS の Array#sort (UTF-16 順) で並べた語を "" で連結する。 */
std::string voice_cache_key(const std::vector<std::string>& terms, int limit);

} // namespace di::flow
