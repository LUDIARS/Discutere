#pragma once
// 上限つきのスレッドプールとタイマー。
// Node の「Promise を投げっぱなしにする」(void promise) は Executor::submit にする。
// キューが満杯なら submit は false を返す (呼び出し側が拒否・縮退を決める)。
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace di {

class Executor {
public:
    Executor(std::string name, std::size_t threads, std::size_t max_queue);
    ~Executor();
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    /** 仕事を積む。停止中・キュー満杯なら false (積まない)。 */
    bool submit(std::function<void()> task);
    /** 受付を止め、走っている仕事の終わりを待つ (積まれたまま未着手の仕事は捨てる)。 */
    void stop();
    std::size_t queued() const;
    std::size_t running() const;
    std::size_t capacity() const { return max_queue_; }

private:
    void loop();
    std::string name_;
    std::size_t max_queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> threads_;
    std::size_t running_ = 0;
    bool stopping_ = false;
};

/**
 * 背景ジョブ用の共有プール (議論の進行・Discord イベント処理・外部取り込み)。
 * 初回呼び出しで作る。スレッド数 8、キュー 256。
 */
Executor& background_executor();

/** setInterval / setTimeout の代わり。1 本の専用スレッドで期限順に起こす。仕事は短くする
 *  (重い仕事は background_executor に渡す)。 */
class Scheduler {
public:
    using Id = std::uint64_t;
    Scheduler();
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Id after(std::chrono::milliseconds delay, std::function<void()> fn);
    Id every(std::chrono::milliseconds interval, std::function<void()> fn, bool run_now = false);
    void cancel(Id id);
    void stop();

private:
    struct Entry;
    void loop();
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<Entry>> entries_;
    Id next_id_ = 1;
    bool stopping_ = false;
    std::thread thread_;
};

/** プロセス全体で 1 つのタイマー。 */
Scheduler& global_scheduler();

/** 停止要求を待てる sleep (true = 停止要求で起きた)。 */
class StopSignal {
public:
    void request_stop();
    bool stop_requested() const;
    bool wait_for(std::chrono::milliseconds duration);

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

} // namespace di
