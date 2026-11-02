#include "flow/paper_review_session.hpp"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>

namespace di::flow {

namespace {

struct ReviewSession {
    std::string state;  // "preparing" | "reviewing"
    std::vector<std::string> buffered;
    bool reproposed = false;
};

std::mutex& state_mutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, ReviewSession>& sessions() {
    static std::map<std::string, ReviewSession> s;
    return s;
}
struct QueueEntry {
    std::shared_ptr<detail::SerialQueue> queue;
    std::size_t users = 0;
};
std::map<std::string, QueueEntry>& tails() {
    static std::map<std::string, QueueEntry> t;
    return t;
}

} // namespace

void reset_review_sessions_for_test() {
    std::lock_guard lock(state_mutex());
    sessions().clear();
    tails().clear();
}

bool open_preparing(const std::string& thread_id) {
    std::lock_guard lock(state_mutex());
    if (sessions().count(thread_id)) return false;
    sessions()[thread_id] = ReviewSession{"preparing", {}, false};
    return true;
}

bool is_preparing(const std::string& thread_id) {
    std::lock_guard lock(state_mutex());
    auto it = sessions().find(thread_id);
    return it != sessions().end() && it->second.state == "preparing";
}

bool has_review_session(const std::string& thread_id) {
    std::lock_guard lock(state_mutex());
    return sessions().count(thread_id) > 0;
}

bool buffer_reply(const std::string& thread_id, const std::string& text) {
    std::lock_guard lock(state_mutex());
    auto it = sessions().find(thread_id);
    if (it == sessions().end() || it->second.state != "preparing") return false;
    it->second.buffered.push_back(text);
    return true;
}

std::vector<std::string> mark_reviewing(const std::string& thread_id) {
    std::lock_guard lock(state_mutex());
    auto it = sessions().find(thread_id);
    if (it == sessions().end()) {
        sessions()[thread_id] = ReviewSession{"reviewing", {}, false};
        return {};
    }
    it->second.state = "reviewing";
    std::vector<std::string> buffered = std::move(it->second.buffered);
    it->second.buffered.clear();
    return buffered;
}

bool claim_repropose(const std::string& thread_id) {
    std::lock_guard lock(state_mutex());
    auto it = sessions().find(thread_id);
    if (it == sessions().end() || it->second.reproposed) return false;
    it->second.reproposed = true;
    return true;
}

void close_review_session(const std::string& thread_id) {
    std::lock_guard lock(state_mutex());
    sessions().erase(thread_id);
}

namespace detail {

struct SerialQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::uint64_t next_ticket = 0;
    std::uint64_t serving = 0;
};

SerialTurn::SerialTurn(const std::string& thread_id) : thread_id_(thread_id) {
    std::uint64_t ticket = 0;
    {
        std::lock_guard lock(state_mutex());
        auto& entry = tails()[thread_id];
        if (!entry.queue) entry.queue = std::make_shared<SerialQueue>();
        ++entry.users;
        queue_ = entry.queue;
        std::lock_guard qlock(queue_->mutex);
        ticket = queue_->next_ticket++;
    }
    std::unique_lock qlock(queue_->mutex);
    queue_->cv.wait(qlock, [&] { return queue_->serving == ticket; });
}

SerialTurn::~SerialTurn() {
    {
        std::lock_guard qlock(queue_->mutex);
        ++queue_->serving;
    }
    queue_->cv.notify_all();
    std::lock_guard lock(state_mutex());
    auto it = tails().find(thread_id_);
    // reset 後に戻ってきた札は別の待ち行列なので触らない
    if (it == tails().end() || it->second.queue != queue_) return;
    if (--it->second.users == 0) tails().erase(it);
}

std::size_t serial_queue_count() {
    std::lock_guard lock(state_mutex());
    return tails().size();
}

} // namespace detail

} // namespace di::flow
