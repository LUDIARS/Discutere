#include "slack/web_api.hpp"

#include <stdexcept>

#include "base/log.hpp"
#include "base/strings.hpp"
#include "infra/http_client.hpp"

namespace di::slack {

std::string truncate_slack(const std::string& s, std::size_t max) {
    if (str::js_length(s) <= max) return s;
    return str::js_slice(s, 0, static_cast<std::int64_t>(max) - 1) + "…";
}

Json slack_post(const std::string& method, const std::string& token, const Json& body, const WebApiOptions& options) {
    http::ClientRequest req;
    req.method = "POST";
    req.url = options.base_url + "/" + method;
    req.headers.set("Authorization", "Bearer " + token);
    req.headers.set("Content-Type", "application/json; charset=utf-8");
    req.body = json_stringify(body);
    req.timeout = options.timeout;
    req.max_response_bytes = options.max_response_bytes;
    req.allow_private_network = options.allow_private_network;
    req.max_redirects = 0;
    auto res = http::fetch(req);
    if (!res.transport_ok) throw std::runtime_error("slack " + method + ": " + res.error);
    JsonLimits limits;
    limits.max_bytes = options.max_response_bytes;
    auto parsed = parse_json(res.body, limits);
    if (!parsed || !parsed->is_object()) return Json{{"ok", false}, {"error", "invalid_json"}};
    return std::move(*parsed);
}

std::string apps_connections_open(const std::string& app_token, const WebApiOptions& options) {
    const Json json = slack_post("apps.connections.open", app_token, Json::object(), options);
    if (!(json.contains("ok") && json["ok"] == true) || !json.contains("url") || !json["url"].is_string())
        throw std::runtime_error("apps.connections.open 失敗: " + json_string(json, "error", "unknown"));
    return json["url"].get<std::string>();
}

PostedMessage post_message(const std::string& bot_token, const PostMessageArgs& args, const WebApiOptions& options) {
    Json body{{"channel", args.channel}, {"text", truncate_slack(args.text)}};
    if (args.threadTs && !args.threadTs->empty()) body["thread_ts"] = *args.threadTs;
    if (args.username && !args.username->empty()) body["username"] = *args.username;
    if (args.iconEmoji && !args.iconEmoji->empty()) body["icon_emoji"] = *args.iconEmoji;
    if (args.blocks) body["blocks"] = Json(*args.blocks);
    const Json json = slack_post("chat.postMessage", bot_token, body, options);
    if (!(json.contains("ok") && json["ok"] == true)) throw std::runtime_error("chat.postMessage 失敗: " + json_string(json, "error", "unknown"));
    return PostedMessage{json_string(json, "ts"), json_opt_string(json, "channel").value_or(args.channel)};
}

void reactions_add(const std::string& bot_token, const ReactionsAddArgs& args, const WebApiOptions& options) {
    const Json json = slack_post("reactions.add", bot_token, Json{{"channel", args.channel}, {"timestamp", args.timestamp}, {"name", args.name}}, options);
    if (!(json.contains("ok") && json["ok"] == true) && json_string(json, "error") != "already_reacted")
        log::warn("  slack-web-api: reactions.add 失敗 (" + args.name + "): " + json_string(json, "error", "unknown"));
}

} // namespace di::slack
