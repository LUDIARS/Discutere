#include "api/web_chat_routes.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>

#include "api/js_query.hpp"
#include "api/web_page.hpp"
#include "base/json.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "core/projection/message_input.hpp"
#include "flow/discussion_paper.hpp"
#include "web_chat/session.hpp"

namespace di::api {

namespace {

std::mutex g_mutex;
WebChatDeps g_deps;

WebChatDeps current_deps() {
    std::lock_guard<std::mutex> lock(g_mutex);
    WebChatDeps d = g_deps;
    if (!d.openCore) d.openCore = open_active_core;
    return d;
}

std::string parse_chat_session_state(const std::optional<std::string>& v) {
    if (v && (*v == "draft" || *v == "live" || *v == "concluded")) return *v;
    return "all";
}

/** Number.MAX_SAFE_INTEGER */
constexpr double kMaxSafeInteger = 9007199254740991.0;

double clamp_int_query(const std::optional<std::string>& v, double fallback, double min, double max) {
    const auto n = js_query::parse_int(v.value_or(""));
    if (!n || !std::isfinite(*n)) return fallback;
    return std::min(max, std::max(min, *n));
}

/** 本文 JSON の上限 (text + author)。 */
constexpr JsonLimits kBodyLimits{kWebChatMaxBody, 1'000, 16};

Json field(const Json& body, const char* key) {
    if (body.is_object() && body.contains(key)) return body[key];
    return Json(nullptr);
}

} // namespace

void set_web_chat_deps(WebChatDeps deps) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_deps = std::move(deps);
}

http::Router web_chat_routes() {
    http::Router router;

    // ─── チャット UI (静的 HTML) ─────────────────────────────────
    router.get("/chat", [](http::Request&) { return http::Response::html(std::string(web_page("api/chat.html"))); });

    router.get("/api/chat/sessions", [](http::Request& c) {
        flow::ListFlowSessionsOptions o;
        o.state = parse_chat_session_state(c.query("state"));
        o.scope = "chat";
        const double limit = clamp_int_query(c.query("limit"), 50, 1, 100);
        const double offset = clamp_int_query(c.query("offset"), 0, 0, kMaxSafeInteger);
        o.limit = static_cast<std::int64_t>(limit);
        o.offset = static_cast<std::int64_t>(offset);
        const auto res = flow::list_flow_sessions(o);
        Json sessions = Json::array();
        for (const auto& r : res.rows) {
            const bool concluded = r.concluded && *r.concluded == 1;
            const std::string st = r.status == "draft" ? "draft" : concluded ? "concluded" : "live";
            Json j = flow::to_json(r);
            j["concluded"] = concluded;
            j["state"] = st;
            sessions.push_back(std::move(j));
        }
        Json body = Json::object();
        body["ok"] = true;
        body["total"] = res.total;
        body["limit"] = o.limit;
        body["offset"] = o.offset;
        body["hasMore"] = offset + static_cast<double>(res.rows.size()) < static_cast<double>(res.total);
        body["sessions"] = sessions;
        return http::Response::json(body);
    });

    // ─── 会話取得 (増分ポーリング) ───────────────────────────────
    router.get("/api/chat/:room/messages", [](http::Request& c) {
        const auto deps = current_deps();
        const auto room = web_chat::sanitize_room_id(c.param("room"));
        const auto since_n = js_query::parse_int(c.query("since").value_or("0"));
        const double since = since_n && *since_n != 0 ? *since_n : 0;
        auto core = deps.openCore();
        web_chat::ReadTranscriptOptions opts;
        opts.sinceTs = since;
        opts.resolveName = deps.resolveSpeakerName;
        Json messages = Json::array();
        for (const auto& m : web_chat::read_room_transcript(*core, deps.workspaceId, room, opts)) messages.push_back(web_chat::to_json(m));
        const bool discussing = web_chat::web_discussion_exists(*core, deps.workspaceId, room);
        core->close();
        Json body = Json::object();
        body["ok"] = true;
        body["room"] = room;
        body["discussing"] = discussing;
        body["messages"] = messages;
        return http::Response::json(body);
    });

    // ─── 人間発話の投入 ──────────────────────────────────────────
    router.post(
        "/api/chat/:room/messages",
        [](http::Request& c) {
            const auto deps = current_deps();
            const auto room = web_chat::sanitize_room_id(c.param("room"));
            auto parsed = c.json(kBodyLimits);
            const Json body = parsed ? *parsed : Json::object();
            // TS は JSON の null で body.text が TypeError (500)。
            if (body.is_null()) return http::Response::text("Internal Server Error", 500);
            const auto text_v = field(body, "text");
            const auto author_v = field(body, "author");
            const std::string text = text_v.is_string() ? str::trim(text_v.get<std::string>()) : "";
            std::string author = "参加者";
            if (author_v.is_string()) {
                const auto trimmed = str::trim(author_v.get<std::string>());
                if (!trimmed.empty()) author = str::js_slice(trimmed, 0, 40);
            }
            if (text.empty()) return http::Response::json(Json{{"ok", false}, {"error", "text required"}}, 400);

            auto core = deps.openCore();
            const auto session_id = web_chat::ensure_web_session(*core, deps.workspaceId, room);
            core::projection::SubmitMessageInput in;
            in.core = core.get();
            in.workspaceId = deps.workspaceId;
            in.sessionId = session_id;
            in.personId = author;
            in.rawContent = text;
            const auto res = core::projection::submit_message(in);

            // スラッシュ風コマンド (utteranceId 無し) はそのまま返す。
            if (res.utteranceId.empty()) {
                core->close();
                Json out = Json::object();
                out["ok"] = true;
                out["room"] = room;
                out["seeded"] = false;
                if (res.commandResult) out["commandResult"] = core::projection::to_json(*res.commandResult);
                return http::Response::json(out);
            }

            // この room でまだ議論が立っていなければ分類器で designGap を起こす。
            // 立っていれば event-bridge が人間発話として進行中議論にミラーし即応させる。
            bool seeded = false;
            if (deps.classifyInboundMessage && !web_chat::web_discussion_exists(*core, deps.workspaceId, room)) {
                discord_hook::DiscordAutoDiscussionInput input;
                input.workspaceId = deps.workspaceId;
                input.guildId = "web";
                input.channelId = room;
                input.sessionId = session_id;
                input.utteranceId = res.utteranceId;
                input.authorId = author;
                input.content = text;
                // Web チャットは本文を主題アンカーにして確実に議題化する (ゲーム名未特定でも開始)。
                input.forumTitle = str::js_slice(text, 0, 80);
                try {
                    seeded = deps.classifyInboundMessage(input).started;
                } catch (const std::exception& e) {
                    log::warn(std::string("  web-chat: classify failed: ") + e.what());
                }
            }
            core->close();
            Json out = Json::object();
            out["ok"] = true;
            out["room"] = room;
            out["utteranceId"] = res.utteranceId;
            out["seeded"] = seeded;
            return http::Response::json(out);
        },
        http::RouteOptions{kWebChatMaxBody});

    return router;
}

} // namespace di::api
