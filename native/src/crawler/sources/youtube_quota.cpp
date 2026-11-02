#include "crawler/sources/youtube_quota.hpp"

#include <algorithm>
#include <stdexcept>

namespace di::crawler::sources {

void QuotaTracker::spend(long long units, const std::string& label) {
    std::lock_guard lock(mutex_);
    if (used_ + units > daily_) {
        throw std::runtime_error("youtube quota exceeded: need " + std::to_string(units) + (label.empty() ? "" : " (" + label + ")") +
                                 ", used " + std::to_string(used_) + "/" + std::to_string(daily_));
    }
    used_ += units;
}

bool QuotaTracker::can_spend(long long units) const {
    std::lock_guard lock(mutex_);
    return used_ + units <= daily_;
}

long long QuotaTracker::spent() const {
    std::lock_guard lock(mutex_);
    return used_;
}

long long QuotaTracker::remaining() const {
    std::lock_guard lock(mutex_);
    return std::max(0LL, daily_ - used_);
}

std::shared_ptr<QuotaTracker> create_quota_tracker(long long daily_quota) { return std::make_shared<QuotaTracker>(daily_quota); }

} // namespace di::crawler::sources
