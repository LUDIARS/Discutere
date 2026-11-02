#pragma once
// persona-engine rule engine (src/persona-engine/engine/engine.ts, Phase 0)。
//
// - tick (既定 1 秒) + event 配信 (fire_event() で外部から駆動)
// - 並列禁止 (同時に 1 発火だけ)、走行中の発火は skip ログ
// - cooldown_sec 内の rule は skip (last_fired_at で判定)
// - runtime kill switch (rulesDisabled())
//
// LLM 呼び出しは LLMClient 経由、議論コンテキストは DiscussionContextProvider 経由。
// tick は di::global_scheduler() で起こし、rule の発火 (LLM 待ち) は background_executor() で走らせる。
// fire_rule / fire_event は呼んだスレッドで発火を待つ (TS の await と同じ)。
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "base/json.hpp"
#include "persona_engine/context_provider.hpp"
#include "persona_engine/db/personas_repo.hpp"
#include "persona_engine/db/rules_repo.hpp"
#include "persona_engine/db/session_fires_repo.hpp"
#include "persona_engine/engine/handler.hpp"
#include "persona_engine/llm/client.hpp"
#include "persona_engine/types.hpp"

namespace di::persona_engine::engine {

/** 外部 event ({kind, sessionId?, payload?})。 */
struct EngineEvent {
    std::string kind;
    std::optional<std::string> sessionId;
    std::optional<Json> payload;
};

struct EngineDeps {
    std::shared_ptr<db::PersonasRepo> personas;
    std::shared_ptr<db::RulesRepo> rules;
    LLMClientPtr llm;
    std::shared_ptr<DiscussionContextProvider> contextProvider;
    std::shared_ptr<Logger> logger;
    /** 議論ペルソナを所属させる workspace_id */
    std::string workspaceId;
    /** event 発火時に渡される sessionId 解決関数 (省略時 null) */
    std::function<std::optional<std::string>(const EngineEvent&)> resolveSessionId;
    /** Runtime kill switch */
    std::function<bool()> rulesDisabled;
    /** tick の周期 ms (default 1000) */
    std::optional<long long> tickMs;
    /** false にすると LLM 呼ばずに skip ログ (テスト用) */
    std::optional<bool> enableLlm;
    /** Safety cap — 同一 session 内の総発火数上限 (未指定なら上限なし)。 */
    std::optional<std::int64_t> maxFiresPerSession;
    /** Safety cap — 同一 session 内で同一 rule の発火数上限 (無限ループ防止。未指定なら上限なし)。 */
    std::optional<std::int64_t> maxFiresPerRulePerSession;
    /** per-session fire counter の保存先。未指定なら in-memory (従来挙動)。 */
    std::shared_ptr<db::SessionFireStore> sessionFireStore;
};

inline constexpr long long DEFAULT_TICK_MS = 1000;

class Engine {
public:
    explicit Engine(EngineDeps deps);
    virtual ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /** tick を始める (二重 start はしない)。 */
    void start();
    /** tick を止める。以後の発火 (tick / fire_rule / fire_event) は走らない。 */
    void stop();
    /** 1 件 manual fire (テスト / debug) */
    void fire_rule(const std::string& rule_id, const std::string& triggered_by,
                   const std::optional<std::string>& session_id = std::nullopt);
    /** 外部 event 通知 (event-driven rules を発火) */
    void fire_event(const EngineEvent& event);
    void set_rules_enabled(bool enabled);
    /** session カウンタを reset (= 議論クローズ後の cap 解放) */
    void reset_session(const std::string& session_id);
    /** tick が動いているか (テスト用)。 */
    bool running() const;

    struct State;

private:
    std::shared_ptr<State> state_;
};

std::unique_ptr<Engine> create_engine(EngineDeps deps);

} // namespace di::persona_engine::engine
