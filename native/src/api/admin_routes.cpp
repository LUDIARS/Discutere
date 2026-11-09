#include "api/admin_routes.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>

#include "base/json.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/kg_registry.hpp"
#include "middleware/auth.hpp"
#include "persona_engine/facilitator/js_value.hpp"
#include "visualize/topic_opinions.hpp"

namespace di::api {

namespace {

struct Bindings {
    std::mutex m;
    std::shared_ptr<persona_engine::PersonaEngineHandle> engine;
    std::shared_ptr<persona_engine::facilitator::Facilitator> facilitator;
    GatewaySweeper sweeper;
    GatewayThreadStarter thread_starter;
    bool runtime_enabled = true;
};

Bindings& bindings() {
    static Bindings b;
    return b;
}

std::shared_ptr<persona_engine::PersonaEngineHandle> engine() {
    std::lock_guard lock(bindings().m);
    return bindings().engine;
}

/** active KG (§12) を開く Core。全 admin route で共通。 */
std::unique_ptr<core::Core> open_core() {
    core::CoreOptions opts;
    opts.sqlitePath = core::resolve_active_kg_path(config::get_config());
    return core::create_core(opts);
}

http::Response not_initialized(const char* what) { return http::Response::json(Json{{"error", std::string(what) + " not initialized"}}, 503); }

/** c.req.json() の結果 (壊れていれば nullopt)。 */
std::optional<Json> body_json(const http::Request& r) { return r.json(); }

/** obj[key] (object でなければ JS と同じく TypeError 相当 → 500)。 */
const Json* member(const Json& body, const char* key) {
    if (body.is_null()) throw std::runtime_error("Cannot read properties of null");
    if (!body.is_object()) return nullptr;
    const auto it = body.find(key);
    return it == body.end() ? nullptr : &*it;
}

double query_number(const http::Request& r, const char* name, double fallback) {
    const auto q = r.query(name);
    if (!q) return fallback;
    const auto n = str::js_number(*q);
    return n ? *n : std::nan("");
}

// 未処理 (open) DesignGap を「却下」する terminal 状態 (status='dismissed')。
constexpr const char* DISMISS_GUARD = "status IS NULL OR status NOT IN ('closed','converged','dismissed')";

} // namespace

void set_persona_engine(std::shared_ptr<persona_engine::PersonaEngineHandle> e) {
    std::lock_guard lock(bindings().m);
    bindings().engine = std::move(e);
}
std::shared_ptr<persona_engine::PersonaEngineHandle> get_persona_engine() { return engine(); }
void set_facilitator(std::shared_ptr<persona_engine::facilitator::Facilitator> f) {
    std::lock_guard lock(bindings().m);
    bindings().facilitator = std::move(f);
}
void set_gateway_sweeper(GatewaySweeper sweeper) {
    std::lock_guard lock(bindings().m);
    bindings().sweeper = std::move(sweeper);
}
void set_gateway_thread_starter(GatewayThreadStarter starter) {
    std::lock_guard lock(bindings().m);
    bindings().thread_starter = std::move(starter);
}

http::Router admin_routes() {
    http::Router router;

    router.post("/admin/rules/enabled", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto e = engine();
        if (!e) return not_initialized("persona-engine");
        const auto body = body_json(c).value_or(Json{{"enabled", true}});
        const Json* v = member(body, "enabled");
        const bool enabled = v && json_truthy(*v);
        e->set_rules_enabled(enabled);
        {
            std::lock_guard lock(bindings().m);
            bindings().runtime_enabled = enabled;
        }
        return http::Response::json(Json{{"ok", true}, {"enabled", enabled}});
    });

    router.post("/admin/session/reset", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto e = engine();
        if (!e) return not_initialized("persona-engine");
        const auto body = body_json(c).value_or(Json{{"sessionId", ""}});
        const Json* v = member(body, "sessionId");
        if (!v || !json_truthy(*v)) return http::Response::json(Json{{"error", "sessionId required"}}, 400);
        const std::string session_id = persona_engine::facilitator::js_string(*v);
        e->reset_session(session_id);
        return http::Response::json(Json{{"ok", true}, {"sessionId", *v}});
    });

    router.post("/admin/gaps/:id/dismiss", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto id = c.param("id");
        auto core = open_core();
        const auto r = core->raw().run(std::string("UPDATE design_gaps SET status = 'dismissed', updated_at = ? WHERE id = ? AND workspace_id = ? AND (") +
                                           DISMISS_GUARD + ")",
                                       {time::now_ms(), id, config::get_config().workspace});
        return http::Response::json(Json{{"ok", true}, {"dismissed", r.changes}});
    });

    // 未処理 DesignGap を一括却下 (バックログ一掃)。
    router.post("/admin/gaps/dismiss-open", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        auto core = open_core();
        const auto r = core->raw().run(
            "UPDATE design_gaps SET status = 'dismissed', updated_at = ? WHERE workspace_id = ? AND (status IS NULL OR status NOT IN "
            "('closed','converged','dismissed','resolved','rejected'))",
            {time::now_ms(), config::get_config().workspace});
        return http::Response::json(Json{{"ok", true}, {"dismissed", r.changes}});
    });

    // 進行中の議論 session を「閉じる」(scene 共有 session を一括終了 + 紐づく gap を closed)。
    router.post("/admin/sessions/:id/close", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto id = c.param("id");
        const auto ws = config::get_config().workspace;
        auto core = open_core();
        auto& raw = core->raw();
        const auto sess = raw.get("SELECT id, scene FROM sessions WHERE id = ? AND workspace_id = ?", {id, ws});
        if (!sess) return http::Response::json(Json{{"error", "session not found"}}, 404);
        const auto now = time::now_ms();
        const auto scene = sess->opt_str("scene");
        const bool has_scene = scene && !scene->empty();

        const auto ended = has_scene
                               ? raw.run("UPDATE sessions SET ended_at = ?, updated_at = ? WHERE workspace_id = ? AND scene = ? AND ended_at IS NULL",
                                         {now, now, ws, *scene})
                                     .changes
                               : raw.run("UPDATE sessions SET ended_at = ?, updated_at = ? WHERE id = ? AND ended_at IS NULL", {now, now, id}).changes;

        const auto gap_rows = raw.all(std::string(R"(SELECT DISTINCT SUBSTR(title, LENGTH('discussion-of-gap:') + 1) AS gap_id
             FROM sessions
            WHERE workspace_id = ? AND title LIKE 'discussion-of-gap:%'
              AND )") + (has_scene ? "scene = ?" : "id = ?"),
                                      {ws, has_scene ? *scene : id}, {100'000, 64u << 20});
        std::int64_t closed = 0;
        for (const auto& g : gap_rows) {
            closed += raw.run(R"(UPDATE design_gaps SET status = 'closed', updated_at = ?
         WHERE id = ? AND workspace_id = ?
           AND (status IS NULL OR status NOT IN ('closed','converged','dismissed','resolved','rejected')))",
                              {now, g.str("gap_id"), ws})
                          .changes;
        }
        return http::Response::json(Json{{"ok", true}, {"endedSessions", ended}, {"closedGaps", closed}});
    });

    // 進行中の議論を「まとめて閉じる」: facilitator.converge_now で LLM まとめ → 【収束】投稿 → gap closed → onConverged。
    router.post("/admin/sessions/:id/converge", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        std::shared_ptr<persona_engine::facilitator::Facilitator> fac;
        {
            std::lock_guard lock(bindings().m);
            fac = bindings().facilitator;
        }
        if (!fac) return not_initialized("facilitator");
        const auto id = c.param("id");
        const auto ws = config::get_config().workspace;
        auto core = open_core();
        auto& raw = core->raw();
        const auto sess = raw.get("SELECT scene FROM sessions WHERE id = ? AND workspace_id = ?", {id, ws});
        if (!sess) return http::Response::json(Json{{"error", "session not found"}}, 404);
        const auto scene = sess->opt_str("scene");
        const bool has_scene = scene && !scene->empty();
        const auto targets = raw.all(std::string(R"(SELECT id, title FROM sessions
          WHERE workspace_id = ? AND title LIKE 'discussion-of-gap:%'
            AND )") + (has_scene ? "scene = ?" : "id = ?"),
                                     {ws, has_scene ? *scene : id}, {100'000, 64u << 20});
        Json results = Json::array();
        std::int64_t converged = 0;
        for (const auto& t : targets) {
            const auto session_id = t.str("id");
            const auto gap_id = str::js_slice(t.str("title"), static_cast<std::int64_t>(str::js_length("discussion-of-gap:")));
            const auto r = fac->converge_now(session_id, gap_id);
            Json item{{"sessionId", session_id}, {"gapId", gap_id}, {"ok", r.ok}};
            if (r.error) item["error"] = *r.error;
            results.push_back(std::move(item));
            if (r.ok) converged += 1;
        }
        return http::Response::json(Json{{"ok", true}, {"converged", converged}, {"results", results}});
    });

    router.get("/admin/status", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto e = engine();
        bool runtime_enabled = true;
        {
            std::lock_guard lock(bindings().m);
            runtime_enabled = bindings().runtime_enabled;
        }
        Json logs = Json::array();
        std::size_t persona_count = 0;
        std::size_t rule_count = 0;
        if (e) {
            persona_count = e->personas().list().size();
            persona_engine::db::RuleListFilter f;
            f.enabled = true;
            rule_count = e->rules().list(f).size();
            for (const auto& l : e->rules().recent_logs(10)) logs.push_back(persona_engine::to_json(l));
        }
        return http::Response::json(Json{{"persona_engine_attached", e != nullptr},
                                         {"rules_runtime_enabled", runtime_enabled},
                                         {"persona_count", persona_count},
                                         {"rule_count", rule_count},
                                         {"recent_rule_logs", logs}});
    });

    // FEATURE ⑤b: 未検知 (👀 無し) 投稿を再スイープして議論の種にする。
    router.post("/admin/seed-sweep", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        GatewaySweeper sweeper;
        {
            std::lock_guard lock(bindings().m);
            sweeper = bindings().sweeper;
        }
        if (!sweeper) return http::Response::json(Json{{"error", "gateway not initialized"}}, 503);
        const auto body = body_json(c).value_or(Json::object());
        const Json* v = member(body, "limit");
        std::optional<double> limit;
        if (v && json_truthy(*v)) {
            const double n = persona_engine::facilitator::js_number_of(v);
            if (n > 0) limit = std::min(100.0, n);  // NaN は比較が偽 → 未指定
        }
        try {
            const auto r = sweeper(limit);
            return http::Response::json(Json{{"ok", true}, {"scanned", r.scanned}, {"seeded", r.seeded}});
        } catch (const std::exception& err) {
            return http::Response::json(Json{{"error", err.what()}}, 500);
        }
    });

    // 受信できなかった既存のフォーラムスレッドを、新規投稿と同じ経路で起動する。
    router.post("/admin/forum/threads/:id/start", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const std::string id = c.param("id");
        if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) {
            return http::Response::json(Json{{"error", "invalid thread id"}}, 400);
        }
        GatewayThreadStarter starter;
        {
            std::lock_guard lock(bindings().m);
            starter = bindings().thread_starter;
        }
        if (!starter) return http::Response::json(Json{{"error", "gateway not initialized"}}, 503);
        try {
            const auto r = starter(id);
            if (!r.queued) return http::Response::json(Json{{"ok", false}, {"reason", r.reason}}, 409);
            return http::Response::json(Json{{"ok", true}});
        } catch (const std::exception& err) {
            return http::Response::json(Json{{"error", err.what()}}, 500);
        }
    });

    router.get("/admin/learning/topics", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto& config = config::get_config();
        visualize::TopicOpinionOptions opts;
        opts.limit = query_number(c, "limit", 40);
        opts.opinionsPerTopic = query_number(c, "opinionsPerTopic", 8);
        auto core = open_core();
        return http::Response::json(visualize::build_topic_opinion_snapshot(core->raw(), config.workspace, opts));
    });

    // PR-F: learning metrics — persona / rule 採用率
    router.get("/admin/metrics/personas", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto e = engine();
        if (!e) return not_initialized("persona-engine");
        // hypothesis status resolver は未提供 (engine 単体)。status 不明として "unknown" を返す。
        const auto metrics = persona_engine::learning::persona_metrics(e->personas(), e->rules(), [](const std::string&) {
            return std::string("unknown");
        });
        Json arr = Json::array();
        for (const auto& m : metrics) arr.push_back(persona_engine::learning::to_json(m));
        return http::Response::json(Json{{"metrics", arr}});
    });

    router.get("/admin/metrics/rules", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto e = engine();
        if (!e) return not_initialized("persona-engine");
        Json arr = Json::array();
        for (const auto& m : persona_engine::learning::rule_metrics(e->rules())) arr.push_back(persona_engine::learning::to_json(m));
        return http::Response::json(Json{{"metrics", arr}});
    });

    // PR-J: Rule Viewer — 全 rule + 補正履歴
    router.get("/admin/rules", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto e = engine();
        if (!e) return not_initialized("persona-engine");
        const bool include_removed = c.query("includeRemoved") == std::optional<std::string>("1");
        const auto& seed_ids = persona_engine::seeds::SEED_RULE_IDS();
        Json arr = Json::array();
        for (const auto& r : e->rules().list()) {
            if (!include_removed && r.enabled != 1) continue;
            auto opt = [](const auto& v) { return v ? Json(*v) : Json(nullptr); };
            Json j = Json::object();
            j["id"] = r.id;
            j["description"] = opt(r.description);
            j["trigger_type"] = r.trigger_type;
            j["tick_sec"] = opt(r.tick_sec);
            j["event_kind"] = opt(r.event_kind);
            j["target"] = opt(r.target);
            j["cooldown_sec"] = r.cooldown_sec;
            j["last_fired_at"] = opt(r.last_fired_at);
            j["enabled"] = r.enabled == 1;
            j["added_at"] = r.added_at;
            j["added_by"] = r.added_by;
            j["removed_at"] = opt(r.removed_at);
            j["removed_by"] = opt(r.removed_by);
            j["removed_reason"] = opt(r.removed_reason);
            j["is_seed"] = seed_ids.count(r.id) > 0;
            j["is_ai_added"] = str::starts_with(r.added_by, "ai:");
            j["is_ai_removed"] = r.removed_by.has_value() && str::starts_with(*r.removed_by, "ai:");
            arr.push_back(std::move(j));
        }
        return http::Response::json(Json{{"rules", arr}});
    });

    router.get("/admin/rules/:id/logs", [](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto e = engine();
        if (!e) return not_initialized("persona-engine");
        const auto id = c.param("id");
        // Math.min(200, Math.max(1, Number(q ?? 50))) — NaN は slice(0, NaN) = 0 件。
        const double n = query_number(c, "limit", 50);
        const double limit = std::isnan(n) ? 0 : std::min(200.0, std::max(1.0, n));
        const auto take = static_cast<std::size_t>(std::trunc(limit));
        Json logs = Json::array();
        for (const auto& l : e->rules().recent_logs(1000)) {
            if (logs.size() >= take) break;
            if (l.rule_id == std::optional<std::string>(id)) logs.push_back(persona_engine::to_json(l));
        }
        return http::Response::json(Json{{"rule_id", id}, {"logs", logs}});
    });

    return router;
}

} // namespace di::api
