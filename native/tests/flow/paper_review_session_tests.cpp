// src/flow/paper-review-session.ts の移植の確認 (tests/flow/paper-review-session.test.ts の観点)。
#include "check.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "flow/paper_review_session.hpp"

using namespace di::flow;

TEST_CASE("openPreparing does not start twice") {
    reset_review_sessions_for_test();
    CHECK(open_preparing("t1"));
    CHECK(!open_preparing("t1"));
    CHECK(is_preparing("t1"));
    CHECK(has_review_session("t1"));
    CHECK(open_preparing("t2"));
    close_review_session("t1");
    CHECK(!has_review_session("t1"));
    CHECK(open_preparing("t1"));
}

TEST_CASE("buffered replies come out in order") {
    reset_review_sessions_for_test();
    CHECK(!buffer_reply("t1", "x"));
    open_preparing("t1");
    CHECK(buffer_reply("t1", "メカニクスに周回を追加"));
    CHECK(buffer_reply("t1", "開始"));
    CHECK_EQ(mark_reviewing("t1"), (std::vector<std::string>{"メカニクスに周回を追加", "開始"}));
    CHECK(!is_preparing("t1"));
    CHECK(!buffer_reply("t1", "y"));
    CHECK(mark_reviewing("t1").empty());
    CHECK(mark_reviewing("t9").empty());
    CHECK(has_review_session("t9"));
}

TEST_CASE("repropose once per session") {
    reset_review_sessions_for_test();
    CHECK(!claim_repropose("t1"));
    open_preparing("t1");
    CHECK(claim_repropose("t1"));
    CHECK(!claim_repropose("t1"));
}

TEST_CASE("run_serial: per thread, in arrival order, failures do not block") {
    reset_review_sessions_for_test();
    std::mutex m;
    std::vector<std::string> order;
    auto push = [&](std::string s) {
        std::lock_guard lock(m);
        order.push_back(std::move(s));
    };
    std::promise<void> gate;
    auto gate_f = gate.get_future().share();
    std::atomic<bool> first_started{false};
    auto first = std::async(std::launch::async, [&] {
        return run_serial("t1", [&] {
            push("first:start");
            first_started = true;
            gate_f.wait();
            push("first:end");
            return 1;
        });
    });
    while (!first_started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto second = std::async(std::launch::async, [&] { return run_serial("t1", [&] { push("second"); return 2; }); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(run_serial("t2", [&] { push("other-thread"); return 3; }), 3);
    {
        std::lock_guard lock(m);
        CHECK_EQ(order, (std::vector<std::string>{"first:start", "other-thread"}));
    }
    gate.set_value();
    CHECK_EQ(first.get(), 1);
    CHECK_EQ(second.get(), 2);
    CHECK_EQ(order, (std::vector<std::string>{"first:start", "other-thread", "first:end", "second"}));

    CHECK_THROWS(run_serial("t3", []() -> int { throw std::runtime_error("boom"); }));
    CHECK_EQ(run_serial("t3", [] { return std::string("ok"); }), std::string("ok"));
    CHECK_EQ(detail::serial_queue_count(), std::size_t{0});
}

TEST_CASE("run_serial keeps arrival order under contention") {
    reset_review_sessions_for_test();
    std::vector<int> seen;
    std::mutex m;
    std::promise<void> gate;
    auto gate_f = gate.get_future().share();
    std::atomic<bool> started{false};
    auto head = std::async(std::launch::async, [&] {
        run_serial("q", [&] {
            started = true;
            gate_f.wait();
        });
    });
    while (!started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::vector<std::future<void>> rest;
    for (int i = 0; i < 8; ++i) {
        rest.push_back(std::async(std::launch::async, [&, i] {
            run_serial("q", [&] {
                std::lock_guard lock(m);
                seen.push_back(i);
            });
        }));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // 届く順を決める
    }
    gate.set_value();
    head.get();
    for (auto& f : rest) f.get();
    CHECK_EQ(seen, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
}

DI_TEST_MAIN()
