#include "flow/voice_cache.hpp"

#include <algorithm>

#include "base/strings.hpp"

namespace di::flow {

namespace {

/** UTF-16 のコード単位列 (JS の文字列比較の順序)。 */
std::u16string to_utf16(std::string_view s) {
    std::u16string out;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto cp = str::next_codepoint(s, i);
        if (cp >= 0x10000) {
            out.push_back(static_cast<char16_t>(0xD800 + ((cp - 0x10000) >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

} // namespace

std::string voice_cache_key(const std::vector<std::string>& terms, int limit) {
    std::vector<std::string> norm;
    for (const auto& t : terms) {
        auto n = str::to_lower(str::trim(t));
        if (!n.empty()) norm.push_back(std::move(n));
    }
    std::stable_sort(norm.begin(), norm.end(),
                     [](const std::string& a, const std::string& b) { return to_utf16(a) < to_utf16(b); });
    std::string key = std::to_string(limit);
    key.push_back('\0');
    for (const auto& n : norm) key += n;
    return key;
}

std::vector<ContextVoice> VoiceCache::lookup(const std::vector<std::string>& terms, int limit) {
    const std::string key = voice_cache_key(terms, limit);
    {
        std::lock_guard lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            ++hits_;
            return it->second;
        }
        ++misses_;
    }
    // 実 lookup はロックの外で行う (KG 検索は遅い)。同じキーが同時に来たら後勝ちで上書きする。
    auto result = fn_ ? fn_(terms, limit) : std::vector<ContextVoice>{};
    std::lock_guard lock(mutex_);
    cache_[key] = result;
    return result;
}

ListExternalVoicesFn VoiceCache::as_fn() {
    return [this](const std::vector<std::string>& terms, int limit) { return lookup(terms, limit); };
}

int VoiceCache::misses() const {
    std::lock_guard lock(mutex_);
    return misses_;
}

int VoiceCache::hits() const {
    std::lock_guard lock(mutex_);
    return hits_;
}

std::shared_ptr<VoiceCache> create_voice_cache(ListExternalVoicesFn fn) { return std::make_shared<VoiceCache>(std::move(fn)); }

} // namespace di::flow
