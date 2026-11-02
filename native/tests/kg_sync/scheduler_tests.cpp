// w3-O: follower 側の自動 pull スケジューラ (src/kg-sync/scheduler.ts)。TS にテストが無いので挙動を確かめる:
//   - auto=false / sourceUrl 未設定なら起動しない (何もしない停止関数)
//   - 起動直後に 1 回 pull (global_scheduler → background_executor)・失敗してもスケジューラは止まらない
//   - 停止後は走らない・ログに URL のクエリ (token が載り得る) を出さない
#include "w3o/w3o_support.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "kg_sync/scheduler.hpp"

using namespace di;

namespace {

bool wait_until(const std::function<bool()>& cond, int ms = 5000) {
    for (int i = 0; i < ms / 10; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

} // namespace

TEST_CASE("auto=false / sourceUrl 未設定は起動しない") {
    std::atomic<int> runs{0};
    config::DiscutereConfig cfg;
    cfg.kgSync.auto_ = false;
    cfg.kgSync.sourceUrl = "https://example.invalid/kg";
    auto stop = kg_sync::start_kg_sync_scheduler(cfg, [&] {
        ++runs;
        return kg_sync::KgPullResult{};
    });
    stop();
    cfg.kgSync.auto_ = true;
    cfg.kgSync.sourceUrl = std::nullopt;
    kg_sync::start_kg_sync_scheduler(cfg, [&] {
        ++runs;
        return kg_sync::KgPullResult{};
    })();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(runs.load(), 0);
}

TEST_CASE("起動直後に 1 回 pull し、失敗はログだけ・停止後は走らない・URL のクエリをログに出さない") {
    std::vector<std::string> logs;
    std::mutex m;
    const int sink = log::add_sink([&](const log::Record& r) {
        std::lock_guard lock(m);
        logs.push_back(std::string(r.message));
    });
    std::atomic<int> runs{0};
    config::DiscutereConfig cfg;
    cfg.kgSync.auto_ = true;
    cfg.kgSync.sourceUrl = "https://example.invalid/kg/migrations?token=secret-in-query";
    cfg.kgSync.intervalHours = 6;
    auto stop = kg_sync::start_kg_sync_scheduler(cfg, [&]() -> kg_sync::KgPullResult {
        ++runs;
        throw std::runtime_error("KG pull failed: HTTP 500");
    });
    CHECK(wait_until([&] {
        std::lock_guard lock(m);
        for (const auto& l : logs)
            if (str::contains(l, "pull failed")) return true;
        return false;
    }));
    stop();
    CHECK_EQ(runs.load(), 1);
    log::remove_sink(sink);
    std::lock_guard lock(m);
    for (const auto& l : logs) CHECK(!str::contains(l, "secret-in-query"));
    bool announced = false;
    for (const auto& l : logs) announced = announced || str::contains(l, "auto pull every 6h from https://example.invalid/kg/migrations");
    CHECK(announced);
}

DI_TEST_MAIN()
