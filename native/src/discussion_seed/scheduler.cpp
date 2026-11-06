#include "discussion_seed/scheduler.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "discussion_seed/seed.hpp"
#include "persona_engine/index.hpp"

namespace di::discussion_seed {

AutoSeedScheduler::AutoSeedScheduler(AutoSeedSchedulerDeps deps) : deps_(std::move(deps)) {
    if (!deps_.core || !deps_.personas) throw std::invalid_argument("auto-seed: core and personas required");
    if (!deps_.logger) deps_.logger = persona_engine::console_logger();
    sources_ = deps_.config.sources.empty() ? std::vector<std::string>{"genre"} : deps_.config.sources;
}

AutoSeedScheduler::~AutoSeedScheduler() { timer_.stop(); }

std::int64_t AutoSeedScheduler::open_headless_count() {
    const auto row = deps_.core->raw().get(R"(SELECT COUNT(*) AS n
           FROM sessions s
           JOIN design_gaps g
             ON g.id = SUBSTR(s.title, LENGTH('discussion-of-gap:') + 1)
          WHERE s.workspace_id = ?
            AND s.title LIKE 'discussion-of-gap:%'
            AND s.scene LIKE 'gap:%'
            AND (g.status IS NULL OR g.status NOT IN ('closed', 'converged')))",
                                           {deps_.workspaceId});
    return row ? row->i64("n") : 0;
}

bool AutoSeedScheduler::has_open_gap_with_title(const std::string& title) {
    return deps_.core->raw()
        .get("SELECT 1 FROM design_gaps WHERE workspace_id = ? AND title = ? AND (status IS NULL OR status NOT IN ('closed', 'converged')) "
             "LIMIT 1",
             {deps_.workspaceId, title})
        .has_value();
}

std::optional<AutoSeedScheduler::Next> AutoSeedScheduler::next_topic() {
    const auto& source = sources_[static_cast<std::size_t>(rotation_ % static_cast<std::int64_t>(sources_.size()))];
    rotation_ += 1;
    if (source == "store-trend") {
        const auto topics = fetch_store_trend_topics(8, deps_.storeTrendSource);
        for (const auto& t : topics)
            if (!has_open_gap_with_title(t.title)) return Next{t, "auto:store-trend"};
        return std::nullopt;
    }
    // default: genre — 被らないジャンルを最大 17 周ぶん探す
    for (int i = 0; i < 17; ++i) {
        auto topic = build_genre_topic(pick_genre_seed(rotation_ + i));
        if (!has_open_gap_with_title(topic.title)) return Next{std::move(topic), "auto:genre"};
    }
    return std::nullopt;
}

std::optional<AutoSeedTriggered> AutoSeedScheduler::trigger() {
    std::lock_guard lock(mutex_);
    if (static_cast<double>(open_headless_count()) >= deps_.config.maxConcurrent) {
        deps_.logger->info(Json{{"open", open_headless_count()}, {"max", deps_.config.maxConcurrent}},
                           "auto-seed skipped (maxConcurrent reached)");
        return std::nullopt;
    }
    const auto next = next_topic();
    if (!next) {
        deps_.logger->info(Json::object(), "auto-seed skipped (no fresh topic)");
        return std::nullopt;
    }
    SeedHeadlessDiscussionArgs args;
    args.core = deps_.core.get();
    args.personas = deps_.personas.get();
    args.workspaceId = deps_.workspaceId;
    args.topic = next->topic;
    args.origin = next->origin;
    const auto seeded = seed_headless_discussion(args);
    deps_.logger->info(Json{{"gap_id", seeded.gapId}, {"title", next->topic.title}, {"origin", next->origin}},
                       "auto-seed started headless discussion");
    return AutoSeedTriggered{seeded.gapId, next->topic.title};
}

void AutoSeedScheduler::start() {
    timer_.start(
        std::chrono::milliseconds(static_cast<long long>(deps_.config.intervalMs)),
        [this] {
            try {
                trigger();
            } catch (const std::exception& err) {
                deps_.logger->warn(Json{{"err", err.what()}}, "auto-seed tick failed");
            }
        },
        [this] { deps_.logger->warn(Json::object(), "auto-seed tick skipped (executor queue full)"); });
}

void AutoSeedScheduler::stop() { timer_.stop(); }

std::shared_ptr<AutoSeedScheduler> create_auto_seed_scheduler(AutoSeedSchedulerDeps deps) {
    return std::make_shared<AutoSeedScheduler>(std::move(deps));
}

} // namespace di::discussion_seed
