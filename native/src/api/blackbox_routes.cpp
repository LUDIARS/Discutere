#include "api/blackbox_routes.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/json.hpp"
#include "base/strings.hpp"
#include "flow/db/connection.hpp"
#include "flow/density_blackbox.hpp"
#include "lib/blackbox/condition.hpp"
#include "middleware/auth.hpp"

namespace di::api {

namespace {

const std::vector<std::string>& RULE_STATES() {
    static const std::vector<std::string> v{"candidate", "trial", "auto", "retired"};
    return v;
}

/** blackbox 判断を持つ既知ドメイン (一覧の既定走査対象)。 */
const std::vector<std::string>& KNOWN_DOMAINS() {
    static const std::vector<std::string> v{flow::DOMAIN_DENSITY};
    return v;
}

/** 本文 JSON の上限 ({verdict} / {state} だけなので小さく)。 */
constexpr JsonLimits kBodyLimits{64u << 10, 1'000, 16};

// TS の module-level `let bbInstance`。engine の読み書き (record_verdict 等) も 1 本ずつにする。
std::mutex g_mutex;
std::optional<lib::blackbox::BlackBox> g_instance;

lib::blackbox::BlackBox& bb_locked(const BlackBoxFactory& factory) {
    if (!g_instance) g_instance = factory ? factory() : flow::make_density_blackbox(flow::db::get_flow_db());
    return *g_instance;
}

/** Number(s) が整数なら int64 に (TS の Number.isInteger)。範囲外は nullopt で 404 と同じ扱いにする。 */
struct ParsedId {
    bool integer = false;
    std::optional<std::int64_t> value;
};
ParsedId parse_decision_id(const std::string& raw) {
    ParsedId out;
    const auto n = str::js_number(raw);
    if (!n || !std::isfinite(*n) || std::trunc(*n) != *n) return out;
    out.integer = true;
    if (*n >= -9.2e18 && *n <= 9.2e18) out.value = static_cast<std::int64_t>(*n);
    return out;
}

/** Math.min(200, Math.max(1, Number(q) || 50)) */
std::int64_t parse_limit(const std::optional<std::string>& q) {
    double n = 50;
    if (q) {
        const auto v = str::js_number(*q);
        if (v && !std::isnan(*v) && *v != 0) n = *v;
    }
    n = std::min(200.0, std::max(1.0, n));
    return static_cast<std::int64_t>(n);
}

/** c.req.json().catch(() => ({})) の読み (壊れていれば空オブジェクト、JSON の null は TS で TypeError=500)。 */
std::optional<Json> read_body(const http::Request& c) {
    auto body = c.json(kBodyLimits);
    if (!body) return Json::object();
    if (body->is_null()) return std::nullopt;
    return body;
}

/** body[key] (オブジェクトでない・無ければ null)。 */
Json field(const Json& body, const char* key) {
    if (body.is_object() && body.contains(key)) return body[key];
    return Json(nullptr);
}

} // namespace

void reset_blackbox_routes() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_instance.reset();
}

http::Router blackbox_routes(BlackBoxFactory factory) {
    http::Router router;

    router.get("/admin/blackbox/decisions", [factory](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        auto domain_q = c.query("domain");
        const std::optional<std::string> domain = domain_q && !domain_q->empty() ? domain_q : std::nullopt;
        const auto limit = parse_limit(c.query("limit"));
        std::lock_guard<std::mutex> lock(g_mutex);
        Json items = Json::array();
        for (const auto& d : bb_locked(factory).ledger->list_pending(domain, limit)) items.push_back(lib::blackbox::to_json(d));
        return http::Response::json(Json{{"items", items}});
    });

    router.post("/admin/blackbox/decisions/:id/verdict", [factory](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto id = parse_decision_id(c.param("id"));
        if (!id.integer) return http::Response::json(Json{{"error", "invalid id"}}, 400);
        const auto body = read_body(c);
        if (!body) return http::Response::text("Internal Server Error", 500);
        const auto verdict = field(*body, "verdict");
        if (!verdict.is_string() || (verdict.get<std::string>() != "ok" && verdict.get<std::string>() != "ng"))
            return http::Response::json(Json{{"error", "verdict must be 'ok' or 'ng'"}}, 400);
        if (!id.value) return http::Response::json(Json{{"error", "decision not found"}}, 404);
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto res = bb_locked(factory).engine->record_verdict(*id.value, verdict.get<std::string>());
        if (!res.ok) return http::Response::json(Json{{"error", "decision not found"}}, 404);
        return http::Response::json(Json{{"ok", true}, {"rule", res.ruleUpdated ? lib::blackbox::to_json(*res.ruleUpdated) : Json(nullptr)}});
    });

    router.get("/admin/blackbox/rules", [factory](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto domain = c.query("domain");
        const std::vector<std::string> domains = domain && !domain->empty() ? std::vector<std::string>{*domain} : KNOWN_DOMAINS();
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& box = bb_locked(factory);
        Json rules = Json::array();
        for (const auto& d : domains) {
            for (const auto& rule : box.engine->list_rules(d)) {
                Json j = lib::blackbox::to_json(rule);
                j["whenText"] = lib::blackbox::describe_condition(rule.when);
                rules.push_back(std::move(j));
            }
        }
        Json stats = Json::array();
        for (const auto& d : domains) stats.push_back(lib::blackbox::to_json(box.stats(d)));
        return http::Response::json(Json{{"rules", rules}, {"stats", stats}});
    });

    router.post("/admin/blackbox/rules/:id/state", [factory](http::Request& c) {
        if (auto guard = middleware::require_admin(c)) return *guard;
        const auto id = c.param("id");
        const auto body = read_body(c);
        if (!body) return http::Response::text("Internal Server Error", 500);
        const auto state = field(*body, "state");
        const auto& states = RULE_STATES();
        if (!state.is_string() || std::find(states.begin(), states.end(), state.get<std::string>()) == states.end())
            return http::Response::json(Json{{"error", "state must be one of " + str::join(states, ",")}}, 400);
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto rule = bb_locked(factory).engine->set_rule_state(id, state.get<std::string>());
        if (!rule) return http::Response::json(Json{{"error", "rule not found"}}, 404);
        return http::Response::json(Json{{"ok", true}, {"rule", lib::blackbox::to_json(*rule)}});
    });

    return router;
}

} // namespace di::api
