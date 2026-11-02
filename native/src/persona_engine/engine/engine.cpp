#include "persona_engine/engine/engine.hpp"

#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>

#include "base/executor.hpp"
#include "base/time.hpp"
#include "persona_engine/engine/prompt_builder.hpp"

namespace di::persona_engine::engine {

struct Engine::State : std::enable_shared_from_this<Engine::State> {
    EngineDeps deps;
    long long tick_ms = DEFAULT_TICK_MS;
    std::shared_ptr<db::SessionFireStore> session_fires;

    mutable std::mutex mutex;
    std::optional<Scheduler::Id> timer;
    bool running = false;  // 発火中 (並列禁止)
    bool stopped = false;
    bool runtime_enabled = true;
    /** 実行待ち・実行中の tick ジョブ数 (executor のキューを tick で埋めない)。 */
    std::atomic<int> tick_jobs{0};

    static std::int64_t now_sec() { return time::now_ms() / 1000; }

    void log_rule(const std::string& rule_id, const std::string& action, const std::string& detail) {
        deps.rules->log({rule_id, action, "engine", detail});
    }

    bool is_disabled() {
        if (!runtime_enabled) return true;
        if (deps.rulesDisabled && deps.rulesDisabled()) return true;
        return false;
    }

    /** 空の sessionId は TS と同じく「無い」(falsy) と扱う。 */
    static bool has_session(const std::optional<std::string>& s) { return s && !s->empty(); }

    std::optional<std::string> check_session_cap(const RuleRow& rule, const std::optional<std::string>& session_id) {
        if (!has_session(session_id)) return std::nullopt;
        if (deps.maxFiresPerSession) {
            const auto total = session_fires->total(*session_id);
            if (total >= *deps.maxFiresPerSession)
                return "session cap reached: " + std::to_string(total) + "/" + std::to_string(*deps.maxFiresPerSession) +
                       " (turn budget)";
        }
        if (deps.maxFiresPerRulePerSession) {
            const auto per_rule = session_fires->rule_count(*session_id, rule.id);
            if (per_rule >= *deps.maxFiresPerRulePerSession)
                return "rule cap reached: " + rule.id + " " + std::to_string(per_rule) + "/" +
                       std::to_string(*deps.maxFiresPerRulePerSession) + " (loop guard)";
        }
        return std::nullopt;
    }

    void try_fire(const RuleRow& rule, const std::string& triggered_by, const std::optional<std::string>& session_id,
                  const std::optional<TriggerEvent>& trigger_event) {
        const std::int64_t now = now_sec();
        {
            std::lock_guard lock(mutex);
            if (stopped) return;
            if (running) {
                log_rule(rule.id, "skip", "engine busy");
                return;
            }
            if (is_disabled()) {
                log_rule(rule.id, "skip", "rules disabled at runtime");
                return;
            }
            if (rule.last_fired_at && *rule.last_fired_at != 0 && now - *rule.last_fired_at < rule.cooldown_sec) {
                return;  // cooldown 内、ログも spam なので無音
            }
            if (auto reason = check_session_cap(rule, session_id)) {
                log_rule(rule.id, "skip", *reason);
                return;
            }
            running = true;
        }
        try {
            deps.rules->set_last_fired(rule.id, now);
            if (has_session(session_id)) session_fires->increment(*session_id, rule.id);
            fire_once(rule, triggered_by, session_id, trigger_event);
        } catch (const std::exception& e) {
            const std::string message = e.what();
            try {
                log_rule(rule.id, "error", message);
            } catch (...) {
            }
            deps.logger->warn(Json{{"rule_id", rule.id}, {"err", message}}, "fire failed");
        } catch (...) {
            deps.logger->warn(Json{{"rule_id", rule.id}, {"err", "unknown error"}}, "fire failed");
        }
        std::lock_guard lock(mutex);
        running = false;
    }

    void fire_once(const RuleRow& rule, const std::string& triggered_by, const std::optional<std::string>& session_id,
                   const std::optional<TriggerEvent>& trigger_event) {
        const std::string persona_id = rule.target.value_or("");
        if (persona_id.empty()) {
            log_rule(rule.id, "skip", "rule.target persona_id not set");
            return;
        }
        const auto persona = deps.personas->get(persona_id);
        if (!persona) {
            log_rule(rule.id, "error", "persona not found: " + persona_id);
            return;
        }
        BuildPromptArgs pargs;
        pargs.rule = &rule;
        pargs.persona = &*persona;
        pargs.workspaceId = deps.workspaceId;
        pargs.sessionId = session_id;
        pargs.contextProvider = deps.contextProvider.get();
        pargs.triggeredBy = triggered_by;
        const auto built = build_prompt(pargs);

        if (deps.enableLlm && !*deps.enableLlm) {
            log_rule(rule.id, "skip", "llm disabled in deps");
            return;
        }

        LLMInvokeArgs largs;
        largs.prompt = built.user;
        largs.system = built.system;
        // worker-pool backend のルーティング用 (他 backend は無視)。
        largs.personaId = persona_id;
        const auto res = deps.llm->invoke(largs);
        if (!res.ok) {
            log_rule(rule.id, "error", "llm error: " + res.error);
            deps.logger->warn(Json{{"rule_id", rule.id}, {"err", res.error}}, "llm invoke failed");
            return;
        }
        {
            // stop() の後に LLM の応答が戻っても議論へ書き込まない (C++ で追加した停止の保証)。
            std::lock_guard lock(mutex);
            if (stopped) {
                log_rule(rule.id, "skip", "engine stopped");
                return;
            }
        }

        HandleActionArgs hargs;
        hargs.ruleId = rule.id;
        hargs.personaId = persona_id;
        hargs.workspaceId = deps.workspaceId;
        hargs.sessionId = session_id;
        hargs.triggerEvent = trigger_event;
        hargs.rawText = res.text;
        hargs.contextProvider = deps.contextProvider.get();
        hargs.rules = deps.rules.get();
        hargs.logger = deps.logger.get();
        handle_action(hargs);
    }

    void tick() {
        {
            std::lock_guard lock(mutex);
            if (stopped) return;
        }
        db::RuleListFilter filter;
        filter.enabled = true;
        filter.trigger_type = "tick";
        const auto ticks = deps.rules->list(filter);
        const std::int64_t now = now_sec();
        for (const auto& r : ticks) {
            if (!r.tick_sec || *r.tick_sec == 0) continue;
            const bool due = !(r.last_fired_at && *r.last_fired_at != 0) || now - *r.last_fired_at >= *r.tick_sec;
            if (due) try_fire(r, "tick", std::nullopt, std::nullopt);
        }
    }
};

Engine::Engine(EngineDeps deps) : state_(std::make_shared<State>()) {
    if (!deps.personas || !deps.rules || !deps.llm || !deps.contextProvider || !deps.logger)
        throw std::invalid_argument("create_engine: missing dependency");
    state_->tick_ms = deps.tickMs.value_or(DEFAULT_TICK_MS);
    // per-session safety counter。sessionId が無い rule (tick rule) はカウントしない (cooldown で十分)。
    state_->session_fires = deps.sessionFireStore ? deps.sessionFireStore : std::make_shared<db::InMemorySessionFireStore>();
    state_->deps = std::move(deps);
}

Engine::~Engine() {
    std::optional<Scheduler::Id> timer;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopped = true;
        timer = state_->timer;
        state_->timer.reset();
    }
    if (timer) global_scheduler().cancel(*timer);
}

void Engine::start() {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->timer) return;
        state_->stopped = false;
        std::weak_ptr<State> weak = state_;
        state_->timer = global_scheduler().every(std::chrono::milliseconds(state_->tick_ms), [weak] {
            auto s = weak.lock();
            if (!s) return;
            {
                std::lock_guard lock(s->mutex);
                if (s->stopped) return;
            }
            // 実行中 1 本 + 待ち 1 本まで (それ以上は次の tick に任せる)。
            if (s->tick_jobs.fetch_add(1) >= 2) {
                s->tick_jobs.fetch_sub(1);
                return;
            }
            const bool queued = background_executor().submit([s] {
                try {
                    s->tick();
                } catch (const std::exception& e) {
                    s->deps.logger->warn(Json{{"err", e.what()}}, "tick failed");
                } catch (...) {
                }
                s->tick_jobs.fetch_sub(1);
            });
            if (!queued) s->tick_jobs.fetch_sub(1);
        });
    }
    state_->deps.logger->info(Json{{"tickMs", state_->tick_ms}}, "persona-engine started");
}

void Engine::stop() {
    std::optional<Scheduler::Id> timer;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopped = true;
        timer = state_->timer;
        state_->timer.reset();
    }
    if (timer) global_scheduler().cancel(*timer);
    state_->deps.logger->info(Json::object(), "persona-engine stopped");
}

bool Engine::running() const {
    std::lock_guard lock(state_->mutex);
    return state_->timer.has_value();
}

void Engine::fire_rule(const std::string& rule_id, const std::string& triggered_by, const std::optional<std::string>& session_id) {
    const auto r = state_->deps.rules->get(rule_id);
    if (!r) {
        state_->deps.logger->warn(Json{{"rule_id", rule_id}}, "fireRule: rule not found");
        return;
    }
    state_->try_fire(*r, triggered_by, session_id, std::nullopt);
}

void Engine::fire_event(const EngineEvent& event) {
    std::optional<std::string> session_id = event.sessionId;
    if (!session_id && state_->deps.resolveSessionId) session_id = state_->deps.resolveSessionId(event);
    db::RuleListFilter filter;
    filter.enabled = true;
    filter.trigger_type = "event";
    for (const auto& r : state_->deps.rules->list(filter)) {
        if (r.event_kind && !r.event_kind->empty() && *r.event_kind != event.kind) continue;
        state_->try_fire(r, "event:" + event.kind, session_id, TriggerEvent{event.kind, event.payload});
    }
}

void Engine::set_rules_enabled(bool enabled) {
    {
        std::lock_guard lock(state_->mutex);
        state_->runtime_enabled = enabled;
    }
    state_->deps.logger->info(Json{{"enabled", enabled}}, "rules runtime switch");
}

void Engine::reset_session(const std::string& session_id) {
    state_->session_fires->reset(session_id);
    state_->deps.logger->info(Json{{"sessionId", session_id}}, "session counter reset");
}

std::unique_ptr<Engine> create_engine(EngineDeps deps) { return std::make_unique<Engine>(std::move(deps)); }

} // namespace di::persona_engine::engine
