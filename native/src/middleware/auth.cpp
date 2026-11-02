#include "middleware/auth.hpp"

namespace di::middleware {
namespace {

/** c.req.header(name): 空の値は無いのと同じ。 */
std::optional<std::string> header_value(const http::Request& request, std::string_view name) {
    auto value = request.header(name);
    if (!value || value->empty()) return std::nullopt;
    return value;
}

/** c.get(name): 未設定は nullopt (空文字は空文字のまま)。 */
std::optional<std::string> context_value(const http::Request& request, std::string_view name) {
    auto it = request.vars.find(std::string(name));
    if (it == request.vars.end()) return std::nullopt;
    return it->second;
}

void set_anonymous(http::Request& request) {
    request.vars["userId"] = "anonymous";
    request.vars["userRole"] = "general";
}

} // namespace

bool is_loopback_request(const http::Request& request) {
    if (request.remote_address.empty()) return false;
    return request.from_loopback();
}

http::Middleware user_context() {
    return [](http::Request& request, const http::Next& next) -> http::Response {
        const auto header_user_id = header_value(request, "X-User-Id");
        const auto header_role = header_value(request, "X-User-Role");
        if (header_user_id) {
            request.vars["userId"] = *header_user_id;
            request.vars["userRole"] = header_role ? *header_role : std::string("general");
        } else if (is_loopback_request(request)) {
            request.vars["userId"] = "loopback-admin";
            request.vars["userRole"] = "admin";
        } else {
            set_anonymous(request);
        }
        return next();
    };
}

http::Middleware require_role(std::string role) {
    return [role = std::move(role)](http::Request& request, const http::Next& next) -> http::Response {
        const std::string current = context_value(request, "userRole").value_or("general");
        if (current != role) return http::Response::json(Json{{"error", "Role '" + role + "' required"}}, 403);
        return next();
    };
}

std::string get_user_id(const http::Request& request) {
    if (auto v = context_value(request, "userId"); v && !v->empty()) return *v;
    if (auto h = header_value(request, "X-User-Id")) return *h;
    return "";
}

std::string get_user_role(const http::Request& request) {
    if (auto v = context_value(request, "userRole"); v && !v->empty()) return *v;
    if (auto h = header_value(request, "X-User-Role")) return *h;
    return "general";
}

std::string get_user_name(const http::Request& request) {
    if (auto v = context_value(request, "userName"); v && !v->empty()) return *v;
    return "";
}

std::optional<std::string> get_user_email(const http::Request& request) { return context_value(request, "userEmail"); }

std::optional<http::Response> require_admin(const http::Request& request) {
    const std::string user_id = get_user_id(request);
    const std::string role = get_user_role(request);
    if (user_id.empty()) return http::Response::json(Json{{"error", "Authentication required"}}, 401);
    if (role != "admin") return http::Response::json(Json{{"error", "admin role required"}}, 403);
    return std::nullopt;
}

} // namespace di::middleware
