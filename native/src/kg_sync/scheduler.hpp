#pragma once
// src/kg-sync/scheduler.ts の移植: follower 側の自動 pull スケジューラ — spec/data/core/KG-SYNC.md。
// config.kgSync.auto かつ sourceUrl がある場合のみ起動。起動直後に 1 回 + intervalHours ごとに共有 KG を pull する。
// 定期実行は di::global_scheduler()、pull 本体 (HTTP + DB) は background_executor() に渡す。
// 前回の pull が終わっていなければ次の回は飛ばす (同時実行は 1 本まで。移植で追加)。
#include <functional>

#include "config.hpp"
#include "kg_sync/pull.hpp"

namespace di::kg_sync {

/** 1 回の pull (テストで差し替え可)。既定は active KG を開いて pull_shared_kg。 */
using KgSyncRunOnce = std::function<KgPullResult()>;

/** 起動して停止関数を返す。起動しない設定なら何もしない停止関数を返す。 */
std::function<void()> start_kg_sync_scheduler(const config::DiscutereConfig& config, KgSyncRunOnce run_once = {});

} // namespace di::kg_sync
