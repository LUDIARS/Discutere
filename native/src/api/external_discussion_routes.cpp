// @implements DI-EXTERNAL-DISCUSSION
#include "api/external_discussion_routes.hpp"

#include <atomic>
#include <exception>

#include "base/crypto.hpp"

namespace di::api {

http::Router external_discussion_routes(std::shared_ptr<external_discussion::ExternalDiscussionService> service, std::string secret) {
    http::Router router;
    auto running = std::make_shared<std::atomic<int>>(0);
    router.body_limit("*", kExternalDiscussionMaxBody);
    router.post(
        "/consider",
        [service, secret, running](http::Request& req) {
            if (secret.empty() || !service) return http::Response::json(Json{{"error", "external discussion unavailable"}}, 503);
            const std::string actual = req.header("authorization").value_or("");
            if (!crypto::constant_time_equal(actual, "Bearer " + secret)) return http::Response::json(Json{{"error", "unauthorized"}}, 401);
            external_discussion::ConversationInput input;
            try {
                const auto body = req.json(JsonLimits{kExternalDiscussionMaxBody, 20'000, 16});
                if (!body) throw std::runtime_error("invalid json");
                input = external_discussion::parse_conversation(*body);
            } catch (...) {
                return http::Response::json(Json{{"error", "invalid conversation"}}, 400);
            }
            if (running->fetch_add(1) >= kExternalDiscussionMaxRunning) {
                running->fetch_sub(1);
                return http::Response::json(Json{{"error", "discussion busy"}}, 429);
            }
            struct Release {
                std::atomic<int>& n;
                ~Release() { n.fetch_sub(1); }
            } release{*running};
            try {
                return http::Response::json(external_discussion::to_json(service->consider(input)));
            } catch (...) {
                return http::Response::json(Json{{"error", "discussion generation failed"}}, 503);
            }
        },
        http::RouteOptions{kExternalDiscussionMaxBody});
    return router;
}

} // namespace di::api
