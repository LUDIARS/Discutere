#pragma once
// 定期実行の小さな補助 (TS の setInterval + running フラグの置き換え。w3-M の局所の補助)。
//
// - 周期の起こしは di::global_scheduler()、仕事そのものは di::background_executor() で走らせる
//   (scheduler のスレッドで重い仕事をしない)。
// - 前回の仕事が終わっていなければその周期は飛ばす (TS の `if (running) return;`)。
// - executor のキューが満杯なら、その周期は飛ばして on_skip を呼ぶ (縮退。次の周期で再挑戦)。
// - stop() は以後の起こしを止め、走っている仕事の終わりを待つ (仕事の中から呼んだときは待たない)。
//   デストラクタも stop() する。
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "base/executor.hpp"

namespace di::persona_engine::facilitator {

class PeriodicTask {
public:
    PeriodicTask() = default;
    ~PeriodicTask() { stop(); }
    PeriodicTask(const PeriodicTask&) = delete;
    PeriodicTask& operator=(const PeriodicTask&) = delete;

    /** 既に動いていれば何もしない (TS の `if (timer) return;`)。run_now で最初の 1 回をすぐ起こす。 */
    void start(std::chrono::milliseconds interval, std::function<void()> work, std::function<void()> on_skip = {},
               bool run_now = false) {
        std::lock_guard lock(mutex_);
        if (id_) return;
        auto state = std::make_shared<State>();
        state->work = std::move(work);
        state->on_skip = std::move(on_skip);
        state_ = state;
        const auto period = std::max(interval, std::chrono::milliseconds(1));
        id_ = global_scheduler().every(
            period,
            [state] {
                {
                    std::lock_guard g(state->m);
                    if (state->stopped || state->running) return;
                    state->running = true;
                }
                const bool queued = background_executor().submit([state] {
                    state->worker = std::this_thread::get_id();
                    try {
                        state->work();
                    } catch (...) {
                        // 仕事の側で握りつぶす前提。ここでは周期を止めない。
                    }
                    std::lock_guard g(state->m);
                    state->worker = std::thread::id();
                    state->running = false;
                    state->cv.notify_all();
                });
                if (!queued) {
                    {
                        std::lock_guard g(state->m);
                        state->running = false;
                        state->cv.notify_all();
                    }
                    if (state->on_skip) state->on_skip();
                }
            },
            run_now);
    }

    void stop() {
        std::shared_ptr<State> state;
        {
            std::lock_guard lock(mutex_);
            if (id_) global_scheduler().cancel(*id_);
            id_.reset();
            state = std::move(state_);
        }
        if (!state) return;
        std::unique_lock g(state->m);
        state->stopped = true;
        if (state->worker == std::this_thread::get_id()) return;  // 仕事の中から止めた
        state->cv.wait(g, [&] { return !state->running; });
    }

    bool started() const {
        std::lock_guard lock(mutex_);
        return id_.has_value();
    }

private:
    struct State {
        std::mutex m;
        std::condition_variable cv;
        bool running = false;
        bool stopped = false;
        std::thread::id worker;
        std::function<void()> work;
        std::function<void()> on_skip;
    };
    mutable std::mutex mutex_;
    std::optional<Scheduler::Id> id_;
    std::shared_ptr<State> state_;
};

} // namespace di::persona_engine::facilitator
