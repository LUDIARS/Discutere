#pragma once
// YouTube Data API quota トラッカー (Phase 1) — src/crawler/sources/youtube-quota.ts の移植。
//
// 無料枠 = 10,000 units/日 をプロセス内で加算管理する。上限超過で can_spend が false。
// search.list は 100 units、list 系 (commentThreads/playlistItems/videos/channels) は 1 unit。
#include <memory>
#include <mutex>
#include <string>

namespace di::crawler::sources {

struct YtCost {
    static constexpr int search = 100;
    static constexpr int list = 1;
};
inline constexpr YtCost YT_COST{};

class QuotaTracker {
public:
    explicit QuotaTracker(long long daily_quota = 10'000) : daily_(daily_quota) {}
    /** units を消費。残量を超える場合は std::runtime_error ("youtube quota exceeded: ...")。 */
    void spend(long long units, const std::string& label = {});
    /** 消費せず可否だけ判定 */
    bool can_spend(long long units) const;
    long long spent() const;
    long long remaining() const;

private:
    long long daily_;
    long long used_ = 0;
    mutable std::mutex mutex_;
};

std::shared_ptr<QuotaTracker> create_quota_tracker(long long daily_quota = 10'000);

} // namespace di::crawler::sources
