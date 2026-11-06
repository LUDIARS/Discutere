#pragma once
// follower 側の同期状態 (source URL ごとの透かし, src/kg-sync/state.ts)。
// 取得済みの最大 updated_at を URL ごとに JSON ファイルに保存し、次回 since に渡して増分のみ取得する。
// ファイル形式: {"sources": {"<url>": {"watermark": n, "lastPulledAt": n, "lastApplied": n}}} (2 字下げ)。
#include <cstddef>
#include <string>

#include "base/json.hpp"

namespace di::kg_sync {

inline constexpr const char* DEFAULT_SYNC_STATE_PATH = "./data/kg-sync-state.json";

struct SourceSyncState {
    double watermark = 0;
    double lastPulledAt = 0;
    double lastApplied = 0;
};

Json to_json(const SourceSyncState& s);

struct KgSyncState {
    /** url → SourceSyncState の object (読んだファイルの中身を保つため JSON のまま持つ)。 */
    Json sources = Json::object();
};

/** 状態ファイルの上限 (大きすぎる・壊れたファイルは空の状態として扱う)。 */
inline constexpr std::size_t kMaxSyncStateBytes = 16u << 20;

/** 読む。無い・壊れている・大きすぎるときは空の状態。 */
KgSyncState read_sync_state(const std::string& state_path = DEFAULT_SYNC_STATE_PATH);
/** 書く (親ディレクトリを作る)。失敗は std::runtime_error。 */
void write_sync_state(const KgSyncState& state, const std::string& state_path = DEFAULT_SYNC_STATE_PATH);
double get_source_watermark(const KgSyncState& state, const std::string& url);
void set_source_state(KgSyncState& state, const std::string& url, const SourceSyncState& next);

} // namespace di::kg_sync
