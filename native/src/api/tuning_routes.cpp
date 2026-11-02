#include "api/tuning_routes.hpp"

#include <mutex>

#include "api/web_page.hpp"
#include "runtime_settings/js_value.hpp"
#include "secrets/youtube_env.hpp"

namespace di::api {
namespace {

namespace js = runtime_settings::js;

std::mutex& binding_mutex() {
    static std::mutex m;
    return m;
}
std::shared_ptr<runtime_settings::RuntimeSettingsStore>& bound_store() {
    static std::shared_ptr<runtime_settings::RuntimeSettingsStore> s;
    return s;
}
std::function<void()>& bound_reseeder() {
    static std::function<void()> f;
    return f;
}

std::shared_ptr<runtime_settings::RuntimeSettingsStore> current_store() {
    std::lock_guard lock(binding_mutex());
    return bound_store();
}

http::Response internal_error() { return http::Response::text("Internal Server Error", 500); }

/**
 * (await c.req.json().catch(() => ({}))) と同じ。壊れた本文は {}。
 * JSON の null は TS ではプロパティ参照で TypeError (500) になるので nullopt を返す。
 */
std::optional<Json> read_body(const http::Request& request) {
    auto parsed = request.json();
    if (!parsed) return Json::object();
    if (parsed->is_null()) return std::nullopt;
    return parsed;
}

const Json* field(const Json& body, const char* key) {
    if (!body.is_object()) return nullptr;
    auto it = body.find(key);
    return it == body.end() ? nullptr : &*it;
}

/** body.text ?? null。文字列以外 (数値など) は TS では text.trim() で TypeError になる → 500。 */
std::optional<std::optional<std::string>> read_text(const Json& body) {
    const Json* text = field(body, "text");
    if (!text || text->is_null()) return std::optional<std::string>();
    if (!text->is_string()) return std::nullopt;
    return std::optional<std::string>(text->get<std::string>());
}

} // namespace

void set_runtime_settings(std::shared_ptr<runtime_settings::RuntimeSettingsStore> store) {
    std::lock_guard lock(binding_mutex());
    bound_store() = std::move(store);
}

void set_rule_reseeder(std::function<void()> fn) {
    std::lock_guard lock(binding_mutex());
    bound_reseeder() = std::move(fn);
}

http::Router tuning_routes() {
    http::Router router;

    router.get("/admin/tuning/data", [](http::Request&) {
        auto store = current_store();
        if (!store) return http::Response::json(Json{{"enabled", false}}, 503);
        Json roles = Json::array();
        for (const auto& v : store->list_role_prompts()) roles.push_back(runtime_settings::to_json(v));
        Json rules = Json::array();
        for (const auto& v : store->list_rule_instructions()) rules.push_back(runtime_settings::to_json(v));
        Json out = Json::object();
        out["enabled"] = true;
        out["facilitator"] = runtime_settings::to_json(store->get_facilitator_tuning());
        out["rolePrompts"] = roles;
        out["ruleInstructions"] = rules;
        out["costRelay"] = runtime_settings::to_json(store->get_cost_relay());
        out["secrets"] = Json{{"youtubeApiKey", secrets::to_json(secrets::get_youtube_api_key_status())}};
        return http::Response::json(out);
    });

    router.put("/admin/tuning/cost-relay", [](http::Request& request) {
        auto store = current_store();
        if (!store) return http::Response::json(Json{{"error", "settings store not ready"}}, 503);
        auto body = read_body(request);
        if (!body) return internal_error();
        Json patch = Json::object();
        if (const Json* v = field(*body, "enabled")) patch["enabled"] = js::truthy(*v);
        for (const char* k : {"anatomiaUrl", "concordiaUrl", "service"})
            if (const Json* v = field(*body, k)) patch[k] = js::to_string(*v);
        const auto effective = store->set_cost_relay(patch);
        return http::Response::json(Json{{"ok", true}, {"costRelay", runtime_settings::to_json(effective)}});
    });

    router.put("/admin/tuning/facilitator", [](http::Request& request) {
        auto store = current_store();
        if (!store) return http::Response::json(Json{{"error", "settings store not ready"}}, 503);
        auto body = read_body(request);
        if (!body) return internal_error();
        Json patch = Json::object();
        for (const char* k : {"tickMs", "idleGapMs", "maxPersonas", "aufhebungTarget"})
            if (const Json* v = field(*body, k)) patch[k] = js::to_number(*v);
        if (const Json* v = field(*body, "convergePolicy")) patch["convergePolicy"] = *v;
        const auto effective = store->set_facilitator_tuning(patch);
        return http::Response::json(Json{{"ok", true}, {"facilitator", runtime_settings::to_json(effective)}});
    });

    router.put("/admin/tuning/role", [](http::Request& request) {
        auto store = current_store();
        if (!store) return http::Response::json(Json{{"error", "settings store not ready"}}, 503);
        auto body = read_body(request);
        if (!body) return internal_error();
        const Json* role = field(*body, "role");
        if (!role || !json_truthy(*role)) return http::Response::json(Json{{"error", "role required"}}, 400);
        auto text = read_text(*body);
        if (!text) return internal_error();
        const std::string name = js::to_string(*role);
        store->set_role_prompt(name, *text);
        return http::Response::json(Json{{"ok", true}, {"role", *role}, {"effective", store->get_role_prompt(name)}});
    });

    router.put("/admin/tuning/rule", [](http::Request& request) {
        auto store = current_store();
        if (!store) return http::Response::json(Json{{"error", "settings store not ready"}}, 503);
        auto body = read_body(request);
        if (!body) return internal_error();
        const Json* rule_id = field(*body, "ruleId");
        if (!rule_id || !json_truthy(*rule_id)) return http::Response::json(Json{{"error", "ruleId required"}}, 400);
        auto text = read_text(*body);
        if (!text) return internal_error();
        const std::string id = js::to_string(*rule_id);
        store->set_rule_instruction(id, *text);
        std::function<void()> reseed;
        {
            std::lock_guard lock(binding_mutex());
            reseed = bound_reseeder();
        }
        if (reseed) reseed();  // 稼働中の rule engine に instructions を反映する。
        return http::Response::json(Json{{"ok", true}, {"ruleId", *rule_id}, {"effective", store->get_rule_instruction(id)}});
    });

    router.get("/admin/tuning", [](http::Request&) { return http::Response::html(std::string(web_page("api/tuning.html"))); });
    return router;
}

} // namespace di::api
