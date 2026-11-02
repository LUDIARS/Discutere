#pragma once
// イベントログ (src/core/events/event-log.ts)。
// appendEvent は events 表への INSERT と索引表への射影 (apply_event_projection) を 1 つの transaction で行い、
// 確定後に events.jsonl へ 1 行追記する (SoT は SQLite の events 表、jsonl は追記だけの写し)。
//
// 移していないもの: Kuzu グラフへの結果整合 (flushKuzu / rebuildKuzu)。常駐サーバは Kuzu グラフを開かない
// (w1-C port-notes)。graph 無しの TS と同じく何もしない。
//
// 上限 (移植で追加): 1 イベントの jsonl 行は kMaxEventLineBytes まで (超えたら何も書かずに std::length_error)。
// jsonl 全体が kMaxEventsJsonlBytes を超えたら追記を止めて warn する (DB への記録は続ける)。
// 読み出し (load_events_since / rebuild) は行数・バイト数と payload の JSON を上限つきで読む。
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "core/db/kuzu_client.hpp"
#include "core/events/event_types.hpp"

namespace di::core::events {

/** jsonl の 1 行 (1 イベント) の上限。 */
inline constexpr std::size_t kMaxEventLineBytes = 8u << 20;
/** jsonl 全体の上限。超えたら追記をやめる (DB の events 表は書き続ける)。 */
inline constexpr std::uintmax_t kMaxEventsJsonlBytes = std::uintmax_t{8} << 30;

/** 読み出しの上限 (DB の値も外部入力として課金する)。 */
struct EventReadLimits {
    std::size_t max_rows = 2'000'000;
    std::size_t max_bytes = 1024u << 20;
    /** 1 イベントの payload_json の上限。 */
    JsonLimits payload = {kMaxEventLineBytes, 500'000, 64};
};

struct AppendEventInput {
    std::optional<std::string> id;
    std::string eventType;
    Json payload = Json::object();
    std::optional<double> createdAt;
};

struct LoadEventsSinceInput {
    std::optional<std::string> eventId;
    std::optional<double> timestamp;
};

class EventLog {
public:
    /** jsonl_path の親ディレクトリを作り、無ければ空のファイルを作る。 */
    EventLog(db::KuzuClient& client, const std::string& jsonl_path = "./data/events.jsonl",
             EventReadLimits limits = {});

    DomainEvent append_event(const AppendEventInput& event);
    std::vector<DomainEvent> load_events_since(const LoadEventsSinceInput& input = {});
    /** SQLite read model を events から再構築する。 */
    void rebuild_from_event_store();
    const std::string& jsonl_path() const { return jsonl_path_; }

private:
    std::vector<DomainEvent> rows_to_events(const std::vector<sql::Row>& rows) const;
    db::KuzuClient& client_;
    std::string jsonl_path_;
    EventReadLimits limits_;
    std::mutex append_mutex_;
    bool jsonl_full_warned_ = false;
};

} // namespace di::core::events
