#include "base/executor.hpp"

#include <algorithm>

#include "base/log.hpp"

namespace di {

Executor::Executor(std::string name, std::size_t threads, std::size_t max_queue)
    : name_(std::move(name)), max_queue_(max_queue) {
    if (threads == 0) threads = 1;
    for (std::size_t i = 0; i < threads; ++i) threads_.emplace_back([this] { loop(); });
}

Executor::~Executor() { stop(); }

bool Executor::submit(std::function<void()> task) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || queue_.size() >= max_queue_) return false;
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void Executor::stop() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_ && threads_.empty()) return;
        stopping_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable() && t.get_id() != std::this_thread::get_id()) t.join();
        else if (t.joinable()) t.detach();
    }
    threads_.clear();
}

std::size_t Executor::queued() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

std::size_t Executor::running() const {
    std::lock_guard lock(mutex_);
    return running_;
}

void Executor::loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            task = std::move(queue_.front());
            queue_.pop_front();
            ++running_;
        }
        try {
            task();
        } catch (const std::exception& e) {
            log::error("[" + name_ + "] background task failed: " + e.what());
        } catch (...) {
            log::error("[" + name_ + "] background task failed");
        }
        std::lock_guard lock(mutex_);
        --running_;
    }
}

Executor& background_executor() {
    static Executor executor("background", 8, 256);
    return executor;
}

struct Scheduler::Entry {
    Id id;
    std::chrono::steady_clock::time_point due;
    std::chrono::milliseconds interval{0};
    std::function<void()> fn;
    bool cancelled = false;
};

Scheduler::Scheduler() : thread_([this] { loop(); }) {}

Scheduler::~Scheduler() { stop(); }

Scheduler::Id Scheduler::after(std::chrono::milliseconds delay, std::function<void()> fn) {
    std::lock_guard lock(mutex_);
    auto e = std::make_shared<Entry>();
    e->id = next_id_++;
    e->due = std::chrono::steady_clock::now() + delay;
    e->fn = std::move(fn);
    entries_.push_back(e);
    cv_.notify_all();
    return e->id;
}

Scheduler::Id Scheduler::every(std::chrono::milliseconds interval, std::function<void()> fn, bool run_now) {
    if (interval.count() <= 0) interval = std::chrono::milliseconds(1000);
    std::lock_guard lock(mutex_);
    auto e = std::make_shared<Entry>();
    e->id = next_id_++;
    e->interval = interval;
    e->due = std::chrono::steady_clock::now() + (run_now ? std::chrono::milliseconds(0) : interval);
    e->fn = std::move(fn);
    entries_.push_back(e);
    cv_.notify_all();
    return e->id;
}

void Scheduler::cancel(Id id) {
    std::lock_guard lock(mutex_);
    for (auto& e : entries_)
        if (e->id == id) e->cancelled = true;
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](const auto& e) { return e->cancelled; }), entries_.end());
    cv_.notify_all();
}

void Scheduler::stop() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            if (!thread_.joinable()) return;
        }
        stopping_ = true;
        entries_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

void Scheduler::loop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        if (entries_.empty()) {
            cv_.wait(lock);
            continue;
        }
        auto next = std::min_element(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) { return a->due < b->due; });
        auto due = (*next)->due;
        if (std::chrono::steady_clock::now() < due) {
            cv_.wait_until(lock, due);
            continue;
        }
        auto entry = *next;
        if (entry->interval.count() > 0) {
            entry->due = std::chrono::steady_clock::now() + entry->interval;
        } else {
            entries_.erase(next);
        }
        auto fn = entry->fn;
        lock.unlock();
        try {
            fn();
        } catch (const std::exception& e) {
            log::error(std::string("[scheduler] task failed: ") + e.what());
        } catch (...) {
            log::error("[scheduler] task failed");
        }
        lock.lock();
    }
}

Scheduler& global_scheduler() {
    static Scheduler scheduler;
    return scheduler;
}

void StopSignal::request_stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
}

bool StopSignal::stop_requested() const {
    std::lock_guard lock(mutex_);
    return stop_;
}

bool StopSignal::wait_for(std::chrono::milliseconds duration) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, duration, [this] { return stop_; });
}

} // namespace di
