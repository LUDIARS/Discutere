#include "api/glab_review_trend_routes.hpp"

#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/strings.hpp"
#include "persona_bridge/bridge_auth.hpp"

namespace di::api {

http::Router glab_review_trend_routes(GlabTrendLoader load_trends, GlabTokenLoader token) {
    if (!load_trends) load_trends = [] { return integrations::load_stored_steam_review_trends(); };
    if (!token) token = [] { return env::get("DISCUTERE_GLAB_REVIEW_TRENDS_TOKEN").value_or(""); };
    http::Router routes;
    routes.get("/integrations/glab/review-trends", [load_trends, token](http::Request& req) {
        auto with_cache = [](http::Response r) {
            r.headers.set("cache-control", "private, no-store");
            return r;
        };
        const std::string expected = token();
        if (str::js_length(expected) < kGlabReviewTrendMinTokenLength)
            return with_cache(http::Response::json(Json{{"ok", false}, {"error", "review_trends_unavailable"}}, 503));
        // /^Bearer ([^\s]+)$/ (persona_bridge の bearer_token と同じ規則)。
        const std::string actual = persona_bridge::bearer_token(req.header("authorization"));
        if (!crypto::constant_time_equal(actual, expected))
            return with_cache(http::Response::json(Json{{"ok", false}, {"error", "unauthorized"}}, 401));
        try {
            Json data = Json::array();
            for (const auto& t : load_trends()) data.push_back(integrations::to_json(t));
            return with_cache(http::Response::json(Json{{"ok", true}, {"source", "discutere"}, {"windowDays", 7}, {"data", std::move(data)}}));
        } catch (...) {
            return with_cache(http::Response::json(Json{{"ok", false}, {"error", "review_trends_unavailable"}}, 503));
        }
    });
    return routes;
}

} // namespace di::api
