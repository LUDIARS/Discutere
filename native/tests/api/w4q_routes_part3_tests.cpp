// w4q_routes のテスト (3/4)。準備は w4q_routes_support.hpp。
#include "w4q_routes_support.hpp"

using namespace w4q_routes_support;

// ─── web-chat ──────────────────────────────────────────────────────────

TEST_CASE("web-chat: POST の入力検証・コマンド・分類器の起動と二重起動の防止") {
    w4q::Env env;
    std::atomic<int> classified{0};
    std::string last_title;
    api::WebChatDeps deps;
    deps.workspaceId = "ws";
    deps.classifyInboundMessage = [&](const discord_hook::DiscordAutoDiscussionInput& in) {
        ++classified;
        last_title = in.forumTitle.value_or("");
        CHECK_EQ(in.guildId, std::string("web"));
        // 議論を立てたことにする (scene=web:<room> の discussion-of-gap session)
        auto c = core::create_core(env.kg, env.dir.file("events.jsonl"));
        c->raw().exec("INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('gx', 'ws', 't', 'open', 1, 1)");
        c->raw().exec("INSERT INTO sessions (id, workspace_id, title, started_at, scene, created_at, updated_at) VALUES ('sgx', 'ws', "
                      "'discussion-of-gap:gx', 1, 'web:room1', 1, 1)");
        c->close();
        return discord_hook::StarterResult{true};
    };
    api::set_web_chat_deps(deps);
    const auto root = root_app(env);

    auto r = call(root, "POST", "/api/chat/room1/messages", Json::object(), R"({"text":"   "})");
    CHECK_EQ(r.status, 400);
    CHECK_EQ(json_stringify(body_json(r)), std::string(R"({"ok":false,"error":"text required"})"));
    CHECK_EQ(call(root, "POST", "/api/chat/room1/messages", Json::object(), "{broken").status, 400);  // 壊れた JSON は {} 扱い
    CHECK_EQ(call(root, "POST", "/api/chat/room1/messages", Json::object(), "null").status, 500);     // TS は null.text の TypeError
    CHECK_EQ(call(root, "POST", "/api/chat/room1/messages", Json::object(), R"({"text":5})").status, 400);

    r = call(root, "POST", "/api/chat/room1/messages", Json::object(), R"({"text":"/nope"})");
    CHECK_EQ(json_stringify(body_json(r)),
             std::string(R"({"ok":true,"room":"room1","seeded":false,"commandResult":{"ok":false,"error":"unknown command: nope"}})"));

    const std::string long_text = str::repeat("あ", 100);
    r = call(root, "POST", "/api/chat/room1/messages", Json::object(),
             json_stringify(Json{{"text", "  " + long_text + "  "}, {"author", "  " + str::repeat("名", 50) + "  "}}));
    CHECK_EQ(r.status, 200);
    auto body = body_json(r);
    CHECK_EQ(body["seeded"].get<bool>(), true);
    CHECK_EQ(classified.load(), 1);
    CHECK_EQ(str::js_length(last_title), std::size_t{80});
    // 表示名は 40 文字まで (個人データは表示名のみ)
    auto c = env.core();
    const auto row = c->raw().get("SELECT speaker_id FROM utterances WHERE id = ?", {body["utteranceId"].get<std::string>()});
    CHECK_EQ(row->str("speaker_id"), str::repeat("名", 40));
    c->close();

    // 議論が立っていれば分類器は呼ばない
    r = call(root, "POST", "/api/chat/room1/messages", Json::object(), R"({"text":"続き"})");
    CHECK_EQ(body_json(r)["seeded"].get<bool>(), false);
    CHECK_EQ(classified.load(), 1);
    // 表示名なしは「参加者」
    c = env.core();
    CHECK_EQ(c->raw().get("SELECT speaker_id FROM utterances WHERE raw_content = '続き'")->str("speaker_id"), std::string("参加者"));
    c->close();

    // 分類器の例外は seeded=false (落ちない)
    deps.classifyInboundMessage = [](const discord_hook::DiscordAutoDiscussionInput&) -> discord_hook::StarterResult {
        throw std::runtime_error("llm down");
    };
    api::set_web_chat_deps(deps);
    r = call(root, "POST", "/api/chat/room2/messages", Json::object(), R"({"text":"別の部屋"})");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(body_json(r)["seeded"].get<bool>(), false);
    api::set_web_chat_deps({});
}

TEST_CASE("web-chat: 本文の上限 (64 KiB ちょうど・+1)・巨大・深い入れ子") {
    w4q::Env env;
    api::WebChatDeps deps;
    deps.workspaceId = "ws";
    api::set_web_chat_deps(deps);
    const auto root = root_app(env);
    CHECK_EQ(root.max_body_for("POST", "/api/chat/room1/messages"), api::kWebChatMaxBody);
    // ちょうど上限の本文は受ける
    const std::string prefix = R"({"text":")";
    const std::string suffix = R"("})";
    const std::string exact = prefix + std::string(api::kWebChatMaxBody - prefix.size() - suffix.size(), 'a') + suffix;
    CHECK_EQ(exact.size(), api::kWebChatMaxBody);
    CHECK_EQ(call(root, "POST", "/api/chat/room1/messages", Json::object(), exact).status, 200);
    // +1 は JSON の上限で読めない → {} 扱い → 400 (サーバは max_body_for で 413 にする)
    const std::string over = prefix + std::string(api::kWebChatMaxBody - prefix.size() - suffix.size() + 1, 'a') + suffix;
    CHECK_EQ(call(root, "POST", "/api/chat/room1/messages", Json::object(), over).status, 400);
    // 深い入れ子も読まない
    CHECK_EQ(call(root, "POST", "/api/chat/room1/messages", Json::object(), std::string(1000, '[') + std::string(1000, ']')).status, 400);
    api::set_web_chat_deps({});
}

TEST_CASE("web-chat: GET の room は正規化、since は parseInt") {
    const auto fx = w4q::load_fixture("routes.json");
    w4q::Env env;
    env.kg_exec(fx["kgSetup"]);
    api::WebChatDeps deps;
    deps.workspaceId = "ws";
    deps.resolveSpeakerName = [](const std::string& id) { return id == "p-a" ? std::string("アリス") : id; };
    api::set_web_chat_deps(deps);
    const auto root = root_app(env);
    auto body = body_json(call(root, "GET", "/api/chat/lobby/messages?since=1500xyz"));
    CHECK_EQ(body["messages"].size(), std::size_t{2});
    CHECK_EQ(body["messages"][0]["speaker"].get<std::string>(), std::string("アリス"));
    body = body_json(call(root, "GET", "/api/chat/" + std::string(200, 'r') + "/messages"));
    CHECK_EQ(str::js_length(body["room"].get<std::string>()), std::size_t{32});
    api::set_web_chat_deps({});
}

DI_TEST_MAIN()
