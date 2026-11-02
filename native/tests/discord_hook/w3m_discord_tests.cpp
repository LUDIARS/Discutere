// w3-M: discord-hook の command-defs / register-commands / level-check / mechanics-check / discussion-bridge /
// directive-handler / monitor-card と level-check/tool・mechanics-check/tool の移植の検証。
// tests/discord-hook/{level-check,mechanics-check}.test.ts・tests/{level,mechanics}-check/tool.test.ts の観点 +
// slash 定義の golden (1 バイト一致)・登録 (guild/global)・投稿ブリッジ・調整指示・状態カード (偽 Discord)。
#include "check.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/test_util.hpp"
#include "db/connection.hpp"
#include "db/repository.hpp"
#include "discord_hook/command_defs.hpp"
#include "discord_hook/directive_handler.hpp"
#include "discord_hook/discussion_bridge.hpp"
#include "discord_hook/facilitator_directives.hpp"
#include "discord_hook/level_check.hpp"
#include "discord_hook/mechanics_check.hpp"
#include "discord_hook/monitor_card.hpp"
#include "discord_hook/poster.hpp"
#include "discord_hook/register_commands.hpp"
#include "discord_hook/slash_interaction.hpp"
#include "level_check/tool.hpp"
#include "mechanics_check/tool.hpp"
#include "persona_engine/llm/mock.hpp"
#include "w1d_support.hpp"

using namespace di;
using namespace di::discord_hook;
using Variant = std::variant<std::string, persona_engine::LLMResult>;

namespace {

const char* kFakeToken = "fake-bot-token-not-real";

/** キー順を無視して比べる。 */
bool same(const Json& a, const Json& b) { return nlohmann::json::parse(a.dump()) == nlohmann::json::parse(b.dump()); }

Json fixture_case(const std::string& file, const std::string& name) {
    for (const auto& c : test::load_fixture(file))
        if (c["name"] == name) return c;
    throw test::Failure("fixture case missing: " + name);
}

persona_engine::LLMClientPtr llm_text(const std::string& text, std::atomic<int>* calls = nullptr) {
    return std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [text, calls](const persona_engine::LLMInvokeArgs&) -> Variant {
            if (calls) ++*calls;
            return text;
        }});
}

struct RecordingResponder : InteractionResponder {
    std::vector<std::string>* lifecycle;
    std::vector<DiagnosticReply> replies;
    bool fail_defer = false;
    bool fail_edit = false;
    explicit RecordingResponder(std::vector<std::string>* l) : lifecycle(l) {}
    void defer_ephemeral() override {
        lifecycle->push_back("defer");
        if (fail_defer) throw std::runtime_error("transport failed");
    }
    void edit_reply(const DiagnosticReply& r) override {
        lifecycle->push_back("edit");
        if (fail_edit) throw std::runtime_error("transport failed");
        replies.push_back(r);
    }
};

SlashInteraction slash(const std::string& name, std::vector<std::pair<std::string, std::optional<std::string>>> opts) {
    SlashInteraction s;
    s.id = "i1";
    s.token = "fake-interaction-token";
    s.applicationId = "app";
    s.commandName = name;
    for (auto& [k, v] : opts)
        if (v) s.options.push_back({k, option_type::String, Json(*v)});
    return s;
}

/** 偽 Discord (登録・interaction 応答・投稿・状態カード)。 */
struct FakeDiscord {
    w1d::FakeServer fake;
    std::mutex m;
    std::vector<std::pair<std::string, std::string>> puts;  // path, body
    std::vector<std::string> callbacks;
    std::vector<std::pair<std::string, std::string>> edits;  // content-type, body
    Json channels = Json::array();
    Json channel_messages = Json::array();
    std::vector<std::string> deleted;
    std::vector<Json> created_messages;
    std::vector<Json> edited_messages;
    std::vector<Json> webhook_posts;
    bool guild_missing = false;
    bool webhook_forbidden = false;
    int next = 500;

    FakeDiscord() {
        auto& r = *fake.router;
        auto put = [this](http::Request& q) {
            std::lock_guard l(m);
            CHECK_EQ(q.header("Authorization").value_or(""), std::string("Bot ") + kFakeToken);
            puts.push_back({q.path, q.body});
            return http::Response::bytes(q.body, "application/json");
        };
        r.put("/applications/:app/commands", put);
        r.put("/applications/:app/guilds/:g/commands", put);
        r.post("/interactions/:id/:token/callback", [this](http::Request& q) {
            std::lock_guard l(m);
            CHECK(!q.header("Authorization"));
            callbacks.push_back(q.body);
            return http::Response::empty(204);
        });
        r.patch("/webhooks/:app/:token/messages/@original", [this](http::Request& q) {
            std::lock_guard l(m);
            edits.push_back({q.header("content-type").value_or(""), q.body});
            return http::Response::json(Json{{"id", "orig"}, {"channel_id", "c"}});
        });
        r.get("/users/@me", [](http::Request&) { return http::Response::json(Json{{"id", "bot"}, {"username", "Di"}, {"bot", true}}); });
        r.get("/guilds/:g", [this](http::Request& q) {
            if (guild_missing) return http::Response::json(Json{{"message", "Unknown Guild"}, {"code", 10004}}, 404);
            return http::Response::json(Json{{"id", q.param("g")}, {"name", "G"}, {"roles", Json::array()}});
        });
        r.get("/guilds/:g/channels", [this](http::Request&) {
            std::lock_guard l(m);
            return http::Response::json(channels);
        });
        r.post("/guilds/:g/channels", [this](http::Request& q) {
            std::lock_guard l(m);
            Json ch = Json::parse(q.body);
            ch["id"] = std::to_string(next++);
            ch["guild_id"] = q.param("g");
            channels.push_back(ch);
            return http::Response::json(ch, 201);
        });
        r.get("/channels/:c/messages", [this](http::Request& q) {
            std::lock_guard l(m);
            CHECK_EQ(q.query("limit").value_or(""), std::string("100"));
            return http::Response::json(channel_messages);
        });
        r.del("/channels/:c/messages/:m", [this](http::Request& q) {
            std::lock_guard l(m);
            deleted.push_back(q.param("m"));
            return http::Response::empty(204);
        });
        r.get("/channels/:c/messages/:m", [this](http::Request& q) {
            std::lock_guard l(m);
            for (const auto& msg : channel_messages)
                if (msg["id"] == q.param("m")) return http::Response::json(msg);
            for (const auto& msg : created_messages)
                if (msg["id"] == q.param("m")) return http::Response::json(msg);
            return http::Response::json(Json{{"message", "Unknown Message"}, {"code", 10008}}, 404);
        });
        r.patch("/channels/:c/messages/:m", [this](http::Request& q) {
            std::lock_guard l(m);
            edited_messages.push_back(Json::parse(q.body));
            return http::Response::json(Json{{"id", q.param("m")}, {"channel_id", q.param("c")}});
        });
        r.post("/channels/:c/messages", [this](http::Request& q) {
            std::lock_guard l(m);
            Json body = Json::parse(q.body);
            Json msg{{"id", std::to_string(next++)}, {"channel_id", q.param("c")}, {"author", {{"id", "bot"}, {"username", "Di"}}}};
            msg["content"] = body.value("content", "");
            created_messages.push_back(msg);
            Json rec = body;
            rec["_channel"] = q.param("c");
            created_messages.back()["_body"] = rec;
            return http::Response::json(msg);
        });
        r.get("/channels/:c", [](http::Request& q) {
            if (q.param("c") == "thread") return http::Response::json(Json{{"id", "thread"}, {"type", 11}, {"parent_id", "forum"}});
            return http::Response::json(Json{{"id", q.param("c")}, {"type", 0}});
        });
        r.get("/channels/:c/webhooks", [this](http::Request&) {
            if (webhook_forbidden) return http::Response::json(Json{{"message", "Missing Permissions"}, {"code", 50013}}, 403);
            return http::Response::json(Json::array({Json{{"id", "w1"}, {"name", "Discutere議論"}, {"token", "fake-webhook-token"}}}));
        });
        r.post("/webhooks/:id/:token", [this](http::Request& q) {
            std::lock_guard l(m);
            Json rec = Json::parse(q.body);
            rec["_query"] = q.query_string;
            webhook_posts.push_back(rec);
            return http::Response::json(Json{{"id", "wm1"}, {"channel_id", "x"}});
        });
        fake.start();
    }

    DiscordApiOptions options() const {
        DiscordApiOptions o;
        o.token = kFakeToken;
        o.base_url = fake.base;
        o.allow_private_network = true;
        return o;
    }
};

/** active KG / main DB / config を一時ディレクトリへ向ける。 */
struct Env {
    test::TempDir dir;
    std::string kg;
    explicit Env(const std::optional<std::string>& bot_token = std::nullopt) {
        kg = dir.file("kg.kuzu");
        env::set_for_test("DISCUTERE_CONFIG", std::string_view(dir.file("no-config.json")));
        env::set_for_test("DISCATIER_KUZU_PATH", std::string_view(kg));
        env::set_for_test("DATABASE_PATH", std::string_view(dir.file("main.db")));
        env::set_for_test("DISCUTERE_DISCORD_BOT_TOKEN",
                          bot_token ? std::optional<std::string_view>(*bot_token) : std::optional<std::string_view>());
        config::reset_config_for_test();
        db::reset_main_db_for_test();
    }
    ~Env() {
        set_discussion_bridge_api_factory(nullptr);
        clear_webhook_cache();
        db::reset_main_db_for_test();
        env::set_for_test("DISCUTERE_DISCORD_BOT_TOKEN", std::nullopt);
        config::reset_config_for_test();
    }
    std::unique_ptr<core::Core> core() { return core::create_core(kg, dir.file("events.jsonl")); }
};

std::string make_session(core::Core& c, const std::string& scene, const std::string& title = "s") {
    core::repositories::SessionInput s;
    s.workspaceId = "knowledge";
    s.title = title;
    s.startedAt = 1;
    s.scene = scene;
    return c.repos.session.create(s);
}

} // namespace

// ─── command-defs / register-commands ─────────────────────────────

TEST_CASE("command-defs: JSON.stringify(DISCORD_COMMAND_DEFS) と 1 バイトも違わない (golden)") {
    const auto expected = fs::read_file(test::fixtures_dir() + "/w3m/command_defs.json", 1u << 20);
    CHECK(expected.has_value());
    CHECK_EQ(json_stringify(DISCORD_COMMAND_DEFS()), *expected);
    // level-check / mechanics-check の 1 番目は必須の String、3 番目は input (TS テストの観点)
    for (const auto& c : DISCORD_COMMAND_DEFS()) {
        if (c["name"] == "level-check" || c["name"] == "mechanics-check") {
            CHECK_EQ(c["options"][0]["type"], Json(option_type::String));
            CHECK_EQ(c["options"][0]["required"], Json(true));
            CHECK_EQ(c["options"][2]["name"], Json("input"));
        }
    }
}

TEST_CASE("register_slash_commands: guild ごと PUT (dm・空は除く) / guild 無しは global / token・app id 必須") {
    FakeDiscord d;
    auto r = register_slash_commands({kFakeToken, "app-1", {"g1", "dm", "", "g2"}}, d.options());
    CHECK_EQ(r.scope, std::string("guild"));
    CHECK_EQ(r.commandCount, DISCORD_COMMAND_DEFS().size());
    CHECK_EQ(json_stringify(to_json(r)),
             json_stringify(Json{{"scope", "guild"}, {"guildIds", {"g1", "g2"}}, {"commandCount", DISCORD_COMMAND_DEFS().size()}}));
    CHECK_EQ(d.puts.size(), std::size_t(2));
    CHECK_EQ(d.puts[0].first, std::string("/applications/app-1/guilds/g1/commands"));
    CHECK_EQ(d.puts[1].first, std::string("/applications/app-1/guilds/g2/commands"));
    CHECK_EQ(d.puts[0].second, json_stringify(DISCORD_COMMAND_DEFS()));
    r = register_slash_commands({kFakeToken, "app-1", {"dm"}}, d.options());
    CHECK_EQ(r.scope, std::string("global"));
    CHECK(r.guildIds.empty());
    CHECK_EQ(d.puts.back().first, std::string("/applications/app-1/commands"));
    try {
        register_slash_commands({"", "app-1", {}}, d.options());
        CHECK(false);
    } catch (const std::invalid_argument& e) {
        CHECK_EQ(std::string(e.what()), std::string("registerSlashCommands: botToken required"));
    }
    try {
        register_slash_commands({kFakeToken, "", {}}, d.options());
        CHECK(false);
    } catch (const std::invalid_argument& e) {
        CHECK_EQ(std::string(e.what()), std::string("registerSlashCommands: applicationId required"));
    }
    CHECK_EQ(d.puts.size(), std::size_t(3));
}

// ─── level-check / mechanics-check (tool + slash adapter) ────────────

TEST_CASE("tool: check_level/mechanics_consistency が Node と同じ結果 (golden、キー順は無視)") {
    for (const auto& c : test::load_fixture("w3m/diagnostic_tools.json")) {
        if (!c.contains("input")) continue;
        const auto name = c["name"].get<std::string>();
        std::atomic<int> calls{0};
        design_diagnostic::DiagnosticDependencies deps{llm_text(c["response"].get<std::string>(), &calls), std::nullopt};
        Json result;
        DiagnosticReply reply;
        if (str::starts_with(name, "level")) {
            const auto r = level_check::check_level_consistency(c["input"], deps);
            result = level_check::to_json(r);
            reply = level_check_reply(r);
        } else {
            const auto r = mechanics_check::check_mechanics_consistency(c["input"], deps);
            result = design_diagnostic::to_json(r);
            reply = mechanics_check_reply(r);
        }
        CHECK_EQ(calls.load(), 1);
        CHECK(same(result, parse_json_or_throw(c["result"].get<std::string>())));
        // 返信: 添付かコードブロックか・ファイル名が同じ。中身はキー順を無視して同じ。
        const auto& files = c["reply"]["files"];
        CHECK_EQ(reply.file.has_value(), !files.empty());
        if (reply.file) {
            CHECK_EQ(reply.file->name, files[0]["name"].get<std::string>());
            CHECK(same(parse_json_or_throw(reply.file->data), parse_json_or_throw(files[0]["data"].get<std::string>())));
            CHECK_EQ(reply.content, c["reply"]["content"].get<std::string>());
        } else {
            const auto expected = c["reply"]["content"].get<std::string>();
            const auto strip = [](const std::string& s) { return s.substr(8, s.size() - 12); };  // ```json\n ... \n```
            CHECK(same(parse_json_or_throw(strip(reply.content)), parse_json_or_throw(strip(expected))));
        }
    }
    const auto shortc = fixture_case("w3m/diagnostic_tools.json", "short-reply");
    CHECK_EQ(mechanics_check_reply({}).content, shortc["reply"]["content"].get<std::string>());
}

TEST_CASE("tool: 検証失敗は LLM を呼ばない・LLM 失敗は秘密を出さない・壊れた応答は invalid_response・期限") {
    const auto lreq = fixture_case("w2j/level_requests.json", "base")["input"];
    const auto lres = fixture_case("w2j/level_responses.json", "valid")["text"].get<std::string>();
    std::atomic<int> calls{0};
    design_diagnostic::DiagnosticDependencies deps{llm_text(lres, &calls), std::nullopt};
    try {
        level_check::check_level_consistency(Json{{"levelText", ""}}, deps);
        CHECK(false);
    } catch (const design_diagnostic::DiagnosticError& e) {
        CHECK(str::contains(e.what(), "levelText"));
    }
    CHECK_EQ(calls.load(), 0);
    auto mreq = fixture_case("w2j/mechanics_requests.json", "base")["input"];
    mreq["playModel"] = Json::object();
    try {
        mechanics_check::check_mechanics_consistency(mreq, deps);
        CHECK(false);
    } catch (const design_diagnostic::DiagnosticError& e) {
        CHECK_EQ(e.code(), std::string("use_level_check"));
    }
    CHECK_EQ(calls.load(), 0);
    auto failing = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [](const persona_engine::LLMInvokeArgs&) -> Variant { return persona_engine::LLMResult::failure("secret-backend-error"); }});
    try {
        level_check::check_level_consistency(lreq, {failing, std::nullopt});
        CHECK(false);
    } catch (const design_diagnostic::DiagnosticError& e) {
        CHECK_EQ(e.code(), std::string("llm_failed"));
        CHECK(!str::contains(e.what(), "secret"));
    }
    try {
        level_check::check_level_consistency(lreq, {llm_text("{}"), std::nullopt});
        CHECK(false);
    } catch (const design_diagnostic::DiagnosticError& e) {
        CHECK_EQ(e.code(), std::string("invalid_response"));
    }
    auto slow = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [](const persona_engine::LLMInvokeArgs&) -> Variant {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            return std::string("late");
        }});
    try {
        level_check::check_level_consistency(lreq, {slow, std::nullopt}, std::chrono::milliseconds(20));
        CHECK(false);
    } catch (const design_diagnostic::DiagnosticError& e) {
        CHECK_EQ(e.code(), std::string("llm_timeout"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(350));  // 打ち切った呼び出しの終わりを待つ
}

TEST_CASE("slash input: 不正な JSON・配列・余計なキー・Discord 上限 (ちょうど/+1)・playModel の扱い") {
    const auto lreq = fixture_case("w2j/level_requests.json", "base")["input"];
    const Json extra{{"references", lreq["references"]}, {"playModel", lreq["playModel"]}};
    const auto parsed = read_level_check_slash_input(lreq["levelText"].get<std::string>(), lreq["baselineText"].get<std::string>(),
                                                     json_stringify(extra));
    CHECK(parsed.playModel.has_value());
    CHECK(same(level_check::to_json(*parsed.playModel), lreq["playModel"]));
    for (const std::string& bad : {std::string("[]"), std::string("{}junk"), std::string(R"({"levelText":"override"})"),
                                  std::string(6'001, 'x')})
        CHECK_THROWS(read_level_check_slash_input("spec", std::nullopt, bad));
    // ちょうど 6000 文字 (UTF-16) は長さでは拒否しない (JSON として読めれば通る)
    const std::string exact = "{\"references\":[]" + std::string(6'000 - 17, ' ') + "}";
    CHECK_EQ(str::js_length(exact), std::size_t(6'000));
    CHECK_EQ(read_level_check_slash_input("spec", std::nullopt, exact).levelText, std::string("spec"));
    // 全角 3000 字 (UTF-16 で 3000) は 6000 バイト超でも長さ内
    const auto mreq = fixture_case("w2j/mechanics_requests.json", "base")["input"];
    const auto m = read_mechanics_check_slash_input(mreq["specText"].get<std::string>(), mreq["baselineText"].get<std::string>(),
                                                    json_stringify(Json{{"references", mreq["references"]}}));
    CHECK(same(design_diagnostic::to_json(m.references->at(0)), mreq["references"][0]));
    try {
        read_mechanics_check_slash_input("spec", std::nullopt, R"({"playModel":{}})");
        CHECK(false);
    } catch (const design_diagnostic::DiagnosticError& e) {
        CHECK_EQ(e.code(), std::string("use_level_check"));
    }
    for (const std::string& bad : {std::string("[]"), std::string("{}junk"), std::string(R"({"specText":"override"})"), std::string(6'001, 'x'),
                                  str::repeat("[", 200) + str::repeat("]", 200)})
        CHECK_THROWS(read_mechanics_check_slash_input("spec", std::nullopt, bad));
}

TEST_CASE("handle_*_command: defer → LLM → edit を 1 回ずつ、失敗は診断失敗の返信、他コマンドは false") {
    const auto lreq = fixture_case("w2j/level_requests.json", "base")["input"];
    const auto lres = fixture_case("w2j/level_responses.json", "valid")["text"].get<std::string>();
    const auto input = json_stringify(Json{{"references", lreq["references"]}, {"playModel", lreq["playModel"]}});
    auto inter = slash("level-check", {{"level", lreq["levelText"].get<std::string>()},
                                       {"baseline", lreq["baselineText"].get<std::string>()},
                                       {"input", input}});
    std::vector<std::string> lifecycle;
    RecordingResponder rec(&lifecycle);
    auto mock = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [&](const persona_engine::LLMInvokeArgs&) -> Variant {
            lifecycle.push_back("llm");
            return lres;
        }});
    design_diagnostic::DiagnosticDependencies deps{mock, std::nullopt};
    CHECK(handle_level_check_command(inter, rec, &deps));
    CHECK_EQ(str::join(lifecycle, ","), std::string("defer,llm,edit"));
    CHECK_EQ(rec.replies.size(), std::size_t(1));
    CHECK(rec.replies[0].file.has_value());
    CHECK_EQ(rec.replies[0].file->name, std::string("level-check.json"));
    const auto attached = parse_json_or_throw(rec.replies[0].file->data);
    std::vector<std::string> keys;
    for (auto it = attached.begin(); it != attached.end(); ++it) keys.push_back(it.key());
    CHECK_EQ(str::join(keys, ","), std::string("logic_differences,design_gap"));
    CHECK_EQ(json_stringify(rec.replies[0].body()["allowed_mentions"]), std::string(R"({"parse":[]})"));

    // LLM 無し / 失敗 (秘密を出さない) / 壊れた応答 → defer, edit のみ・診断失敗
    auto secret = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{
        [](const persona_engine::LLMInvokeArgs&) -> Variant { return persona_engine::LLMResult::failure("secret-token"); }});
    design_diagnostic::DiagnosticDependencies d_secret{secret, std::nullopt};
    design_diagnostic::DiagnosticDependencies d_bad{llm_text("bad JSON"), std::nullopt};
    const std::vector<const design_diagnostic::DiagnosticDependencies*> variants{nullptr, &d_secret, &d_bad};
    for (const auto* deps2 : variants) {
        lifecycle.clear();
        RecordingResponder r2(&lifecycle);
        CHECK(handle_level_check_command(inter, r2, deps2));
        CHECK_EQ(str::join(lifecycle, ","), std::string("defer,edit"));
        CHECK(str::contains(r2.replies[0].content, "診断失敗"));
        CHECK(!str::contains(r2.replies[0].content, "secret-token"));
        CHECK(!r2.replies[0].file.has_value());
    }
    // 他のコマンドは触らない
    lifecycle.clear();
    RecordingResponder r3(&lifecycle);
    CHECK(!handle_level_check_command(slash("another-command", {}), r3, &deps));
    CHECK(!handle_mechanics_check_command(inter, r3, &deps));
    CHECK(lifecycle.empty());
    // editReply の失敗は呼び出し側へ。defer の失敗では LLM を呼ばない
    std::atomic<int> calls{0};
    design_diagnostic::DiagnosticDependencies counted{llm_text(lres, &calls), std::nullopt};
    RecordingResponder fe(&lifecycle);
    fe.fail_edit = true;
    CHECK_THROWS(handle_level_check_command(inter, fe, &counted));
    CHECK_EQ(calls.load(), 1);
    RecordingResponder fd(&lifecycle);
    fd.fail_defer = true;
    CHECK_THROWS(handle_level_check_command(inter, fd, &counted));
    CHECK_EQ(calls.load(), 1);

    // mechanics: playModel 入力は LLM を呼ばずに /level-check を案内
    const auto mreq = fixture_case("w2j/mechanics_requests.json", "base")["input"];
    std::atomic<int> mcalls{0};
    design_diagnostic::DiagnosticDependencies md{llm_text("{}", &mcalls), std::nullopt};
    lifecycle.clear();
    RecordingResponder r4(&lifecycle);
    CHECK(handle_mechanics_check_command(slash("mechanics-check", {{"spec", mreq["specText"].get<std::string>()}, {"input", R"({"playModel":{}})"}}),
                                         r4, &md));
    CHECK_EQ(mcalls.load(), 0);
    CHECK(str::contains(r4.replies[0].content, "/level-check"));
}

TEST_CASE("RestInteractionResponder: 偽 Discord に deferred(ephemeral) と @original 編集 (添付は multipart) を送る") {
    FakeDiscord d;
    DiscordApi api(d.options());
    const auto lreq = fixture_case("w2j/level_requests.json", "base")["input"];
    const auto lres = fixture_case("w2j/level_responses.json", "valid")["text"].get<std::string>();
    const Json raw{{"id", "i1"},
                   {"token", "fake-interaction-token"},
                   {"application_id", "app"},
                   {"type", 2},
                   {"guild_id", "g"},
                   {"channel_id", "c"},
                   {"member", {{"user", {{"id", "u1"}}}}},
                   {"data",
                    {{"name", "level-check"},
                     {"options", Json::array({Json{{"name", "level"}, {"type", 3}, {"value", lreq["levelText"]}},
                                              Json{{"name", "baseline"}, {"type", 3}, {"value", lreq["baselineText"]}},
                                              Json{{"name", "input"}, {"type", 3},
                                                   {"value", json_stringify(Json{{"references", lreq["references"]}, {"playModel", lreq["playModel"]}})}}})}}}};
    const auto inter = slash_interaction_from_json(raw);
    CHECK_EQ(inter.userId.value_or(""), std::string("u1"));
    CHECK_EQ(inter.get_string("level").value_or(""), lreq["levelText"].get<std::string>());
    CHECK(!inter.get_string("missing").has_value());
    CHECK(!inter.get_boolean("level").has_value());
    RestInteractionResponder responder(api, inter);
    design_diagnostic::DiagnosticDependencies deps{llm_text(lres), std::nullopt};
    CHECK(handle_level_check_command(inter, responder, &deps));
    CHECK_EQ(d.callbacks.size(), std::size_t(1));
    CHECK_EQ(d.callbacks[0], std::string(R"({"type":5,"data":{"flags":64}})"));
    CHECK_EQ(d.edits.size(), std::size_t(1));
    CHECK(str::starts_with(d.edits[0].first, "multipart/form-data"));
    CHECK(str::contains(d.edits[0].second, "level-check.json"));
    CHECK(str::contains(d.edits[0].second, R"("allowed_mentions":{"parse":[]})"));
    CHECK_THROWS(slash_interaction_from_json(Json{{"id", "x"}}));
    CHECK_THROWS(slash_interaction_from_json(Json::array()));
}

// ─── discussion-bridge ───────────────────────────────────────────

TEST_CASE("discussion-bridge: headless は skipped・scene 不正・token 無し・webhook 投稿・thread・bot への fallback") {
    FakeDiscord d;
    {
        Env env;  // token 無し
        auto c = env.core();
        const auto headless = make_session(*c, "gap:g1");
        auto r = post_discussion_to_discord({c.get(), "knowledge", headless, "utterance", "司会 結", "本文", true});
        CHECK(!r.ok);
        CHECK(r.skipped);
        CHECK_EQ(json_stringify(to_json(r)), std::string(R"J({"ok":false,"skipped":true,"reason":"session.scene is not discord-bound (headless)"})J"));
        CHECK(post_discussion_to_discord({c.get(), "knowledge", "missing", "utterance", "x", "y", true}).skipped);
        const auto bad = make_session(*c, "discord:1");
        CHECK_EQ(post_discussion_to_discord({c.get(), "knowledge", bad, "utterance", "x", "y", true}).reason.value_or(""),
                 std::string("session.scene channel parse failed"));
        const auto ok_scene = make_session(*c, "discord:1/2");
        r = post_discussion_to_discord({c.get(), "knowledge", ok_scene, "utterance", "x", "y", true});
        CHECK(!r.ok);
        CHECK(!r.skipped);
        CHECK_EQ(r.reason.value_or(""), std::string("no discord bot token (channel_monitors / config どちらも未設定)"));
    }
    {
        Env env(std::string("config-token-not-real"));
        std::vector<std::string> tokens;
        set_discussion_bridge_api_factory([&](const std::string& token) {
            tokens.push_back(token);
            auto o = d.options();
            o.token = token;
            return std::make_shared<DiscordApi>(o);
        });
        // channel_monitors の discord 登録 (guild 一致) を優先する
        db::MonitorRepo().create(Json{{"id", "m1"},
                                      {"workspaceId", "knowledge"},
                                      {"platform", "discord"},
                                      {"channelId", "c"},
                                      {"channelName", "n"},
                                      {"botToken", "monitor-token-not-real"},
                                      {"botWorkspaceId", "1"},
                                      {"createdBy", "t"}});
        auto c = env.core();
        const auto s = make_session(*c, "discord:1/thread");
        auto r = post_discussion_to_discord({c.get(), "knowledge", s, "utterance", "費田 渋", "それは違う。次へ!", true});
        CHECK(r.ok);
        CHECK_EQ(r.channelId.value_or(""), std::string("thread"));
        CHECK_EQ(r.messageId.value_or(""), std::string("wm1"));
        CHECK_EQ(tokens.back(), std::string("monitor-token-not-real"));
        CHECK_EQ(d.webhook_posts.back()["username"], Json("費田 渋"));
        CHECK_EQ(d.webhook_posts.back()["content"], Json(humanize_for_discord("それは違う。次へ!")));
        CHECK(str::contains(d.webhook_posts.back()["_query"].get<std::string>(), "thread_id=thread"));
        // 別 guild は monitor を使わず config の token
        const auto s2 = make_session(*c, "discord:7/chan");
        clear_webhook_cache();
        d.webhook_forbidden = true;  // webhook 権限なし → bot 直接投稿 (名前を本文先頭に)
        r = post_discussion_to_discord({c.get(), "knowledge", s2, "utterance", "論者", "意見", true});
        CHECK(r.ok);
        CHECK_EQ(tokens.back(), std::string("config-token-not-real"));
        CHECK_EQ(d.created_messages.back()["content"], Json("**論者**\n" + humanize_for_discord("意見")));
        // viaWebhook=false は bot 名義でそのまま
        r = post_discussion_to_discord({c.get(), "knowledge", s2, "hypothesis", "論者", "仮説", false});
        CHECK(r.ok);
        CHECK_EQ(d.created_messages.back()["content"], Json(humanize_for_discord("仮説")));
        // token は結果に出ない
        CHECK(!str::contains(json_stringify(to_json(r)), "not-real"));
        d.webhook_forbidden = false;
    }
}

// ─── directive-handler ───────────────────────────────────────────

TEST_CASE("directive-handler: 空文は使い方・議論なしは案内・進行中の gap に保存して了解を返す") {
    Env env;
    auto r = handle_directive_message({"knowledge", "1", "2", "   ", std::nullopt});
    CHECK(!r.stored);
    CHECK(str::contains(r.reply, "進行の調整指示を受け付けます"));
    r = handle_directive_message({"knowledge", "1", "2", "もっと簡単に", std::string("u1")});
    CHECK(!r.stored);
    CHECK(str::contains(r.reply, "進行中の議論が見つかりませんでした"));
    auto c = env.core();
    core::repositories::DesignGapInput g;
    g.workspaceId = "knowledge";
    g.title = "議題";
    g.status = "open";
    const auto gap = c->repos.designGap.create(g);
    make_session(*c, "discord:1/2", "discussion-of-gap:" + gap);
    c.reset();
    r = handle_directive_message({"knowledge", "1", "2", "  もっと簡単に  ", std::string("u1")});
    CHECK(r.stored);
    CHECK_EQ(r.gapId.value_or(""), gap);
    CHECK_EQ(r.reply, std::string("🛠️ 了解です。「もっと簡単に」 を進行に反映します。 これからの発言とまとめに効いてきます。"));
    c = env.core();
    const auto list = list_facilitator_directives(c->raw(), "knowledge", gap);
    CHECK_EQ(list.size(), std::size_t(1));
    CHECK_EQ(list[0], std::string("もっと簡単に"));
    // 巨大な指示は上限 (300 UTF-16) で切って保存
    r = handle_directive_message(c->raw(), {"knowledge", "1", "2", std::string(100'000, 'x'), std::nullopt});
    CHECK(r.stored);
    CHECK_EQ(str::js_length(list_facilitator_directives(c->raw(), "knowledge", gap).back()), DIRECTIVE_TEXT_CAP);
}

// ─── monitor-card ────────────────────────────────────────────────

TEST_CASE("monitor-card: embed が Node と同じ (golden)") {
    for (const auto& c : test::load_fixture("w3m/monitor_embeds.json")) {
        const Json body{{"embeds", Json::array({build_monitor_embed(c["active"].get<std::int64_t>(), c["pending"].get<std::int64_t>(),
                                                                    c["at"].get<std::int64_t>())})}};
        CHECK_EQ(json_stringify(body), c["json"].get<std::string>());
    }
}

TEST_CASE("monitor-card: カテゴリとチャンネルを作り、カードを送り、以後は編集・重複カードは削除・guild 不在は何もしない") {
    FakeDiscord d;
    Env env;
    {
        auto c = env.core();
        make_session(*c, "discord:1/2");
    }
    auto api = std::make_shared<DiscordApi>(d.options());
    MonitorCard card({api, "knowledge", "g"});
    card.tick();
    CHECK_EQ(d.channels.size(), std::size_t(2));
    CHECK_EQ(d.channels[0]["name"], Json("状態"));
    CHECK_EQ(d.channels[0]["type"], Json(channel_type::GuildCategory));
    CHECK_EQ(d.channels[1]["name"], Json("discutere-monitor"));
    CHECK_EQ(d.channels[1]["parent_id"], d.channels[0]["id"]);
    CHECK_EQ(d.created_messages.size(), std::size_t(1));
    const auto& embed = d.created_messages[0]["_body"]["embeds"][0];
    CHECK_EQ(embed["fields"][1]["value"], Json("1"));
    CHECK_EQ(embed["fields"][0]["value"], Json("アクティブ"));
    // 2 回目: 同じカードを編集 (チャンネルは作り直さない)
    card.tick();
    CHECK_EQ(d.channels.size(), std::size_t(2));
    CHECK_EQ(d.created_messages.size(), std::size_t(1));
    CHECK_EQ(d.edited_messages.size(), std::size_t(1));

    // 再起動相当: 既存のカードが 2 枚 (新しい方を残し古い方を消す)、他人のメッセージは無視
    d.channel_messages = Json::array({
        Json{{"id", "1000"}, {"channel_id", "c"}, {"author", {{"username", "u"}, {"id", "bot"}}}, {"embeds", Json::array({Json{{"title", "🟢 Discutere Monitor"}}})}},
        Json{{"id", "3000"}, {"channel_id", "c"}, {"author", {{"username", "u"}, {"id", "bot"}}}, {"embeds", Json::array({Json{{"title", "🟢 Discutere Monitor"}}})}},
        Json{{"id", "9000"}, {"channel_id", "c"}, {"author", {{"username", "u"}, {"id", "human"}}}, {"embeds", Json::array({Json{{"title", "Discutere Monitor"}}})}},
        Json{{"id", "8000"}, {"channel_id", "c"}, {"author", {{"username", "u"}, {"id", "bot"}}}, {"embeds", Json::array()}},
    });
    MonitorCard card2({api, "knowledge", "g"});
    card2.tick();
    CHECK_EQ(d.deleted.size(), std::size_t(1));
    CHECK_EQ(d.deleted[0], std::string("1000"));
    CHECK_EQ(d.edited_messages.size(), std::size_t(2));
    CHECK_EQ(d.created_messages.size(), std::size_t(1));

    d.guild_missing = true;
    MonitorCard card3({api, "knowledge", "g"});
    card3.tick();
    CHECK_EQ(d.edited_messages.size(), std::size_t(2));
    CHECK_EQ(d.created_messages.size(), std::size_t(1));
    d.guild_missing = false;

    // start: 即時 1 回 + 周期、stop 後は止まる
    d.channel_messages = Json::array();
    MonitorCard card4({api, "knowledge", "g"});
    auto sent = [&] {
        std::lock_guard l(d.m);
        return d.created_messages.size() + d.edited_messages.size();
    };
    const auto before = sent();
    card4.start(20);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (sent() < before + 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    card4.stop();
    CHECK(sent() >= before + 2);
}

DI_TEST_MAIN()
