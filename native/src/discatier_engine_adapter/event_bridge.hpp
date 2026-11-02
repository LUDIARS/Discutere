#pragma once
// Discatier events table → persona-engine.fire_event への配線 (PR-D, src/discatier-engine-adapter/event-bridge.ts)。
//
// Discatier Core は events を SQLite に append するだけで「リスナー」機構を持たない。
// ここで定期 polling して新着 event を監視し、persona-engine に流す。
//
// - 1 秒 (default) ごとに di::global_scheduler() で起き、poll 本体は background_executor() で走らせる
//   (fire_event は LLM を待つので scheduler のスレッドを塞がない。走行中の poll があれば今回は飛ばす)
// - event.kind を持って engine.fire_event({kind, sessionId, payload}) を発火
// - sessionId は payload.sessionId か、gap-related なら gap-id 紐付け session を解決
// - 終結イベント (HypothesisIntegrated / HypothesisRejected) は engine.reset_session で safety cap を解放
//
// 議論セッション概念: design gap 1 つに対し 1 つの persistent session (title = "discussion-of-gap:<gapId>")。
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/executor.hpp"
#include "base/json.hpp"
#include "core/index.hpp"
#include "persona_engine/engine/engine.hpp"
#include "persona_engine/index.hpp"

namespace di::discatier_engine_adapter {

struct DiscatierEventRow {
    std::string id;
    std::string event_type;
    std::string payload_json;
    double created_at = 0;
};

/** bridge が engine に求める操作 (TS の PersonaEngineHandle の一部。テストではスタブを渡す)。 */
struct BridgeEngine {
    std::function<void(const persona_engine::engine::EngineEvent&)> fireEvent;
    std::function<void(const std::string&)> resetSession;
    /** learning loop (record_hypothesis_outcome) 用。無ければ記録を飛ばして on_error に流す。 */
    persona_engine::db::PersonasRepo* personas = nullptr;
    persona_engine::db::RulesRepo* rules = nullptr;
};

/** PersonaEngineHandle を BridgeEngine にする (engine は bridge より長く生きること)。 */
BridgeEngine bridge_engine_of(const std::shared_ptr<persona_engine::PersonaEngineHandle>& engine);

struct EventBridgeOptions {
    std::string workspaceId;
    /** polling 周期 ms (default 1000) */
    std::optional<long long> pollMs;
    /** 終結とみなす event types (default: HypothesisIntegrated / HypothesisRejected) */
    std::optional<std::vector<std::string>> closingEventTypes;
    /** ログ出力 (default: log::warn) */
    std::function<void(const std::string& error, const DiscatierEventRow* ev)> onError;
};

/** 1 回の poll で読む event の上限 (移植で追加。残りは次の poll で読む)。 */
inline constexpr std::size_t kMaxEventsPerPoll = 5000;
/** payload_json / evidence_json の上限 (DB の値も外部入力)。 */
inline constexpr JsonLimits kEventPayloadLimits{8u << 20, 500'000, 64};

class EventBridge : public std::enable_shared_from_this<EventBridge> {
public:
    EventBridge(core::Core& core, BridgeEngine engine, EventBridgeOptions options);
    ~EventBridge();
    EventBridge(const EventBridge&) = delete;
    EventBridge& operator=(const EventBridge&) = delete;

    /** polling を始める (二重 start はしない)。 */
    void start();
    /** polling を止め、走行中の poll の終わりを待つ。 */
    void stop();
    /** test 用: 現在の lastSeen を返す */
    double last_seen_at() const;
    /** test 用: 手動 1 回 poll (呼んだスレッドで行う。別の poll が走行中なら 0)。dispatch した件数。 */
    int poll_once();

private:
    void dispatch_one(const DiscatierEventRow& row);
    std::optional<std::string> resolve_session_id(const std::string& event_type, const std::optional<Json>& payload);
    void handle_human_utterance(const std::optional<Json>& payload);
    std::optional<std::string> lookup_gap_id_for_hypothesis(const std::string& hypothesis_id);
    std::string ensure_gap_discussion_session(const std::string& gap_id);
    std::string resolve_gap_discussion_scene(const std::string& gap_id);
    void report(const std::string& error, const DiscatierEventRow* ev);

    core::Core& core_;
    BridgeEngine engine_;
    EventBridgeOptions options_;
    long long poll_ms_;
    std::vector<std::string> closing_;

    mutable std::mutex state_mutex_;
    double last_seen_ = 0;
    std::optional<Scheduler::Id> timer_;
    bool stopped_ = false;
    /** poll の直列化 (TS の polling フラグ)。 */
    std::mutex poll_mutex_;
};

/** createEventBridge。返した bridge は stop() してから core / engine を閉じること。 */
std::shared_ptr<EventBridge> create_event_bridge(core::Core& core, BridgeEngine engine, EventBridgeOptions options);

/** transport bound な scene か (Discord = `discord:` / Web チャット = `web:`)。 */
bool is_bound_scene(const std::optional<std::string>& scene);

/** payload_json を object として読む (配列・壊れた JSON・上限超過は nullopt)。 */
std::optional<Json> parse_event_payload(std::string_view raw);

} // namespace di::discatier_engine_adapter
