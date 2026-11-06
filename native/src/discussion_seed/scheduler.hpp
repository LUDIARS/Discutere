#pragma once
// 自動シード議論スケジューラ (#64/#65, src/discussion-seed/scheduler.ts)。
//
// 定期的に種 (ジャンル / ストアトレンド) から headless 議論を 1 つ立てる。
// 駆動は server で常駐している facilitator に任せる (= ここは「種まき」だけ)。
// 同時に開く headless 議論を maxConcurrent で抑え、暴走を防ぐ。
// 定期実行は global_scheduler() で起こし、仕事 (Steam 取得 + DB) は background_executor() で走らせる。
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/index.hpp"
#include "discussion_seed/store_trends.hpp"
#include "persona_engine/db/personas_repo.hpp"
#include "persona_engine/facilitator/periodic.hpp"
#include "persona_engine/types.hpp"

namespace di::discussion_seed {

struct AutoSeedConfig {
    double intervalMs = 0;
    double maxConcurrent = 0;
    std::vector<std::string> sources;
};

struct AutoSeedSchedulerDeps {
    std::shared_ptr<core::Core> core;
    std::shared_ptr<persona_engine::db::PersonasRepo> personas;
    std::string workspaceId;
    AutoSeedConfig config;
    /** 省略時は console_logger()。 */
    std::shared_ptr<persona_engine::Logger> logger;
    /** ストアトレンドの取得先 (テストで偽サーバへ向ける注入点)。 */
    StoreTrendSource storeTrendSource;
};

struct AutoSeedTriggered {
    std::string gapId;
    std::string title;
};

class AutoSeedScheduler {
public:
    explicit AutoSeedScheduler(AutoSeedSchedulerDeps deps);
    ~AutoSeedScheduler();
    AutoSeedScheduler(const AutoSeedScheduler&) = delete;
    AutoSeedScheduler& operator=(const AutoSeedScheduler&) = delete;

    void start();
    void stop();
    /** 1 回分を即時実行 (手動トリガ / テスト)。立てた gap を返す (skip 時は nullopt)。 */
    std::optional<AutoSeedTriggered> trigger();

private:
    struct Next {
        SeedTopic topic;
        std::string origin;
    };
    std::int64_t open_headless_count();
    bool has_open_gap_with_title(const std::string& title);
    std::optional<Next> next_topic();

    AutoSeedSchedulerDeps deps_;
    std::vector<std::string> sources_;
    std::mutex mutex_;  // rotation と trigger の直列化 (TS は単一スレッド)
    std::int64_t rotation_ = 0;  // ジャンル / source を順に回すためのカウンタ
    persona_engine::facilitator::PeriodicTask timer_;
};

std::shared_ptr<AutoSeedScheduler> create_auto_seed_scheduler(AutoSeedSchedulerDeps deps);

} // namespace di::discussion_seed
