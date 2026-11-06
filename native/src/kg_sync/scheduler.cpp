#include "kg_sync/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>

#include "base/executor.hpp"
#include "base/json.hpp"
#include "base/log.hpp"
#include "base/url.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"

namespace di::kg_sync {

namespace {

/** ログ用: クエリ (token が載り得る) を落とした URL。読めなければ "(invalid url)"。 */
std::string url_for_log(const std::string& raw) {
    const auto u = url::parse(raw);
    if (!u) return "(invalid url)";
    return u->origin() + u->path;
}

} // namespace

std::function<void()> start_kg_sync_scheduler(const config::DiscutereConfig& config, KgSyncRunOnce run_once) {
    const auto& ks = config.kgSync;
    if (!ks.auto_ || !ks.sourceUrl || ks.sourceUrl->empty()) {
        log::info("  kg-sync: scheduler skipped (auto=false または sourceUrl 未設定)");
        return [] {};
    }
    if (!run_once) {
        const auto kg_path = core::resolve_active_kg_path(config);
        KgPullOptions opts;
        opts.url = *ks.sourceUrl;
        opts.statePath = ks.stateFile;
        opts.token = ks.token;
        run_once = [kg_path, opts] {
            core::CoreOptions co;
            co.sqlitePath = kg_path;
            auto core = core::create_core(co);
            struct Closer {
                core::Core& c;
                ~Closer() {
                    try {
                        c.close();
                    } catch (...) {
                    }
                }
            } closer{*core};
            return pull_shared_kg(core->raw(), opts);
        };
    }

    // run_once は共有ポインタで持つ (std::function を値で取り込むと、静的解析が MSVC の
    // std::function の実装を読み違えてメモリリークと誤検知するため)。
    auto pull = std::make_shared<const KgSyncRunOnce>(std::move(run_once));
    auto running = std::make_shared<std::atomic<bool>>(false);
    auto stopped = std::make_shared<std::atomic<bool>>(false);
    auto tick = [pull, running, stopped] {
        if (stopped->load()) return;
        bool expected = false;
        if (!running->compare_exchange_strong(expected, true)) {
            log::warn("  kg-sync: previous pull still running; skipped");
            return;
        }
        const bool queued = background_executor().submit([pull, running] {
            try {
                const auto r = (*pull)();
                log::info("  kg-sync: pulled applied=" + js_number_to_string(r.applied) + " skipped=" + js_number_to_string(r.skipped) +
                          " ignored=" + js_number_to_string(r.ignored) + " watermark=" + js_number_to_string(r.toWatermark));
            } catch (const std::exception& e) {
                log::warn(std::string("  kg-sync: pull failed: ") + e.what());
            } catch (...) {
                log::warn("  kg-sync: pull failed");
            }
            running->store(false);
        });
        if (!queued) {
            running->store(false);
            log::warn("  kg-sync: background queue is full; pull skipped");
        }
    };

    const double hours = std::max(1.0, ks.intervalHours);
    const auto interval = std::chrono::milliseconds(static_cast<long long>(hours * 60 * 60 * 1000));
    // 起動直後に 1 回 (run_now) + 以後 intervalHours ごと。
    const auto id = global_scheduler().every(interval, tick, true);
    log::info("  kg-sync: auto pull every " + js_number_to_string(ks.intervalHours) + "h from " + url_for_log(*ks.sourceUrl));
    return [id, stopped] {
        stopped->store(true);
        global_scheduler().cancel(id);
    };
}

} // namespace di::kg_sync
