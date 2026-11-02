// w4-Q: discord-hook/command-router.ts の移植の検証。
// tests/discord-hook/command-router.test.ts の移植 + admin allowlist の境界 (未設定 / 空 / 一致 / 不一致)・
// command_defs との一致 (登録だけ・handler だけを防ぐ)・interaction JSON の入口・平文/フォーラムの取り込み・
// golden (Node の routeSlashCommand の返信文と一致)。
#include "check.hpp"
#include "w4q_support.hpp"

#include <algorithm>
#include <atomic>
#include <set>

#include "base/strings.hpp"
#include "discord_hook/command_defs.hpp"
#include "discord_hook/command_router.hpp"
#include "persona_engine/index.hpp"
#include "persona_engine/llm/mock.hpp"

using namespace di;
using namespace di::discord_hook;

namespace {

class NoopProvider : public persona_engine::DiscussionContextProvider {
public:
    std::vector<persona_engine::ContextHypothesis> list_active_hypotheses(const std::string&, int) override { return {}; }
    std::vector<persona_engine::ContextGap> list_recent_gaps(const std::string&, int) override { return {}; }
    std::vector<persona_engine::ContextUtterance> list_recent_utterances(const std::string&, const std::string&, int) override { return {}; }
    std::string propose_hypothesis(const persona_engine::ProposeHypothesisInput&) override { return ""; }
    std::string post_utterance(const persona_engine::PostUtteranceInput&) override { return ""; }
};

std::shared_ptr<persona_engine::PersonaEngineHandle> make_engine(const std::string& pe_path) {
    persona_engine::PersonaEngineOptions o;
    o.db = sql::Database::open(pe_path);
    o.llm = std::make_shared<persona_engine::MockLLMClient>(std::vector<persona_engine::MockResponder>{});
    o.contextProvider = std::make_shared<NoopProvider>();
    o.workspaceId = "ws";
    o.enableLlm = false;
    return persona_engine::create_persona_engine(o);
}

CommandRouterDeps deps_with(const w4q::Env& env, std::vector<std::string> admin_ids,
                            std::shared_ptr<persona_engine::PersonaEngineHandle> engine = nullptr) {
    CommandRouterDeps d;
    d.workspaceId = "ws";
    d.adminIds = std::move(admin_ids);
    d.getEngine = [engine] { return engine; };
    const auto kg = env.kg;
    const auto events = env.dir.file("events.jsonl");
    d.openCore = [kg, events] { return core::create_core(kg, events); };
    // fire-and-forget はテストでは同じスレッドで走らせる
    d.submitBackground = [](std::function<void()> fn) {
        fn();
        return true;
    };
    return d;
}

InboundSlashCommand slash(const std::string& name, const std::optional<std::string>& user = std::string("admin1"), std::string args = "") {
    InboundSlashCommand c;
    c.name = name;
    c.argsText = std::move(args);
    c.userId = user;
    c.guildId = "g1";
    c.channelId = "ch1";
    return c;
}

DiscordInboundMessage message(const std::string& channel, const std::string& content, const std::string& id = "m1") {
    DiscordInboundMessage m;
    m.id = id;
    m.channelId = channel;
    m.author = {"u1", "alice", std::nullopt};
    m.content = content;
    m.timestamp = "1970-01-01T00:00:00.000Z";
    return m;
}

std::int64_t count(w4q::Env& env, const std::string& sql) {
    auto c = env.core();
    const auto n = c->raw().get(sql)->i64("n");
    c->close();
    return n;
}

} // namespace

// ─── command-router.test.ts ─────────────────────────────────────────────

TEST_CASE("1. adminIds 未設定 → admin slash は全 deny (安全 default)") {
    w4q::Env env;
    auto engine = make_engine(env.dir.file("pe.db"));
    for (const auto& name : ADMIN_SLASH_COMMANDS()) {
        const auto r = route_slash_command(slash(name), deps_with(env, {}, engine));
        CHECK(r.ephemeral);
        CHECK(str::contains(r.content, "disabled") || str::contains(r.content, "無効"));
    }
    // 空文字だけの allowlist でも userId 無しは通らない
    const auto r = route_slash_command(slash("discutere-status", std::nullopt), deps_with(env, {""}, engine));
    CHECK(str::contains(r.content, "admin only"));
}

TEST_CASE("2. 非 admin user / userId 無し / 空文字 → reject") {
    w4q::Env env;
    auto engine = make_engine(env.dir.file("pe.db"));
    std::atomic<int> backups{0};
    auto d = deps_with(env, {"admin1"}, engine);
    d.triggerBackup = [&backups] {
        ++backups;
        return backup::BackupResult{};
    };
    d.triggerPersonaGenerate = [&backups](const std::optional<std::string>&) {
        ++backups;
        return PersonaGenerateResult{};
    };
    for (const auto& name : ADMIN_SLASH_COMMANDS()) {
        for (const auto& user : std::vector<std::optional<std::string>>{std::string("intruder"), std::nullopt, std::string(""),
                                                                         std::string("ADMIN1"), std::string("admin1 ")}) {
            const auto r = route_slash_command(slash(name, user), d);
            CHECK_EQ(r.content, std::string("⚠️ admin only"));
            CHECK(r.ephemeral);
        }
    }
    CHECK_EQ(backups.load(), 0);
}

TEST_CASE("3. admin + status → persona/rule 件数") {
    w4q::Env env;
    auto engine = make_engine(env.dir.file("pe.db"));
    const auto r = route_slash_command(slash("discutere-status"), deps_with(env, {"other", "admin1"}, engine));
    const auto personas = engine->personas().list().size();
    persona_engine::db::RuleListFilter f;
    f.enabled = true;
    const auto rules = engine->rules().list(f).size();
    CHECK_EQ(r.content, "🤖 persona-engine\n personas: `" + std::to_string(personas) + "`\n active rules: `" + std::to_string(rules) + "`");
}

TEST_CASE("4-6. kill enabled=false / true / option 無し") {
    w4q::Env env;
    auto engine = make_engine(env.dir.file("pe.db"));
    auto d = deps_with(env, {"admin1"}, engine);
    auto c = slash("discutere-kill");
    c.enabledOption = false;
    CHECK(str::contains(route_slash_command(c, d).content, "DISABLED"));
    c.enabledOption = true;
    CHECK(str::contains(route_slash_command(c, d).content, "ENABLED"));
    c.enabledOption.reset();
    CHECK(str::contains(route_slash_command(c, d).content, "usage:"));
}

TEST_CASE("7. engine 未初期化 → not initialized") {
    w4q::Env env;
    CHECK(str::contains(route_slash_command(slash("discutere-status"), deps_with(env, {"admin1"})).content, "not initialized"));
    auto d = deps_with(env, {"admin1"});
    d.getEngine = nullptr;
    CHECK(str::contains(route_slash_command(slash("discutere-status"), d).content, "not initialized"));
}

TEST_CASE("8. route_inbound_message は非許可チャンネルを早期 return (core に触れない)") {
    w4q::Env env;
    auto d = deps_with(env, {"admin1"});
    std::atomic<int> opened{0};
    d.openCore = [&opened]() -> std::unique_ptr<core::Core> {
        ++opened;
        throw std::runtime_error("must not open");
    };
    const auto r1 = route_inbound_message(message("ch-not-allowed", "ふつうの発言"), "g1", d);
    CHECK(!r1.ingested);
    CHECK(!r1.seed);
    d.discussionChannelIds = {"ch-other"};
    const auto r2 = route_inbound_message(message("ch-not-allowed", "ふつうの発言"), "g1", d);
    CHECK(!r2.ingested);
    // 親が空文字でも許可しない
    CHECK(!route_inbound_message(message("ch-not-allowed", "x"), "g1", d, std::string("")).ingested);
    CHECK_EQ(opened.load(), 0);
}

TEST_CASE("9. /discutere-discussions — 議論一覧 (state 絞り込み) を ephemeral で返す") {
    const auto fx = w4q::load_fixture("slash.json");
    w4q::Env env;
    env.flow_exec(fx["flowSetup"]);
    const auto d = deps_with(env, {});
    const auto all = route_slash_command(slash("discutere-discussions", std::nullopt, ""), d);
    CHECK(all.ephemeral);
    CHECK(str::contains(all.content, "ローグライトの周回設計"));
    CHECK(str::contains(all.content, "#1 **ローグライトの周回設計**"));
    const auto draft = route_slash_command(slash("discutere-discussions", std::nullopt, "draft"), d);
    CHECK(str::contains(draft.content, "📝下書き"));
    CHECK(!str::contains(draft.content, "ローグライトの周回設計"));
}

// ─── golden ──────────────────────────────────────────────────────────────

TEST_CASE("golden: routeSlashCommand の返信文が Node と一致") {
    const auto fx = w4q::load_fixture("slash.json");
    w4q::Env env;
    env.kg_exec(fx["kgSetup"]);
    env.flow_exec(fx["flowSetup"]);
    for (const auto& c : fx["slash"]) {
        const auto& in = c["input"];
        std::vector<std::string> admins{"admin1"};
        if (in.contains("adminIds")) admins = in["adminIds"].get<std::vector<std::string>>();
        auto d = deps_with(env, admins);
        InboundSlashCommand cmd;
        cmd.name = in["name"].get<std::string>();
        cmd.argsText = in["argsText"].get<std::string>();
        if (in.contains("userId") && in["userId"].is_string()) cmd.userId = in["userId"].get<std::string>();
        if (in.contains("enabledOption")) cmd.enabledOption = in["enabledOption"].get<bool>();
        cmd.guildId = "G1";
        cmd.channelId = "C1";
        const auto r = route_slash_command(cmd, d);
        const auto expected = w4q::mask_uuids(c["reply"]["content"].get<std::string>());
        if (w4q::mask_uuids(r.content) != expected)
            throw test::Failure(cmd.name + " " + cmd.argsText + "\n  actual:   " + r.content + "\n  expected: " + expected);
        CHECK_EQ(r.ephemeral, c["reply"]["ephemeral"].get<bool>());
    }
}

// ─── command_defs との一致 ───────────────────────────────────────────────

TEST_CASE("command_defs と route の name が一致する (登録だけ / handler だけを防ぐ)") {
    std::set<std::string> defs;
    for (const auto& d : DISCORD_COMMAND_DEFS()) defs.insert(d["name"].get<std::string>());
    const auto in = [](const std::vector<std::string>& v, const std::string& n) { return std::find(v.begin(), v.end(), n) != v.end(); };
    // 登録された全コマンドに handler がある
    for (const auto& name : defs)
        CHECK(in(ROUTER_SLASH_COMMANDS(), name) || in(DEDICATED_SLASH_COMMANDS(), name) || in(DISCATIER_COMMANDS(), name));
    // router / gateway 専用 handler の全コマンドが登録されている
    for (const auto& name : ROUTER_SLASH_COMMANDS()) CHECK(defs.count(name) == 1);
    for (const auto& name : DEDICATED_SLASH_COMMANDS()) CHECK(defs.count(name) == 1);
    for (const auto& name : ADMIN_SLASH_COMMANDS()) CHECK(in(ROUTER_SLASH_COMMANDS(), name));
    // Discatier として登録したものは submit_message が "unknown command" を返さない
    w4q::Env env;
    const auto d = deps_with(env, {});
    for (const auto& name : defs) {
        if (!in(DISCATIER_COMMANDS(), name)) continue;
        const auto r = route_slash_command(slash(name, std::string("u1")), d);
        CHECK(!str::contains(r.content, "unknown command"));
    }
}

// ─── interaction JSON の入口 ─────────────────────────────────────────────

namespace {
class RecordingReplier : public SlashReplier {
public:
    std::vector<SlashReply> replies;
    void reply(const SlashReply& r) override { replies.push_back(r); }
};

Json interaction(const std::string& name, const Json& options, const Json& user = Json{{"id", "admin1"}, {"username", "a"}}) {
    return Json{{"id", "i1"},
                {"token", "fake-interaction-token"},
                {"application_id", "app"},
                {"type", 2},
                {"guild_id", "g1"},
                {"channel_id", "ch1"},
                {"member", Json{{"user", user}}},
                {"data", Json{{"name", name}, {"options", options}}}};
}
} // namespace

TEST_CASE("interaction JSON: argsText (stringifyOptionValue) / enabled / guild・channel の既定") {
    auto cmd = inbound_slash_command_from_json(interaction(
        "propose", Json::array({Json{{"name", "statement"}, {"type", 3}, {"value", "a b"}}, Json{{"name", "n"}, {"type", 4}, {"value", 1.5}},
                                Json{{"name", "e"}, {"type", 5}, {"value", false}}, Json{{"name", "x"}, {"type", 3}, {"value", ""}},
                                Json{{"name", "o"}, {"type", 3}, {"value", Json{{"k", 1}}}}})));
    CHECK_EQ(cmd.argsText, std::string(R"(a b 1.5 false {"k":1})"));
    CHECK(!cmd.enabledOption);
    CHECK_EQ(cmd.guildId, std::string("g1"));
    cmd = inbound_slash_command_from_json(interaction("discutere-kill", Json::array({Json{{"name", "enabled"}, {"type", 5}, {"value", true}}})));
    CHECK(cmd.enabledOption && *cmd.enabledOption);
    // 型違いの enabled は undefined (discord.js の getBoolean が投げる → catch)
    cmd = inbound_slash_command_from_json(interaction("discutere-kill", Json::array({Json{{"name", "enabled"}, {"type", 3}, {"value", "true"}}})));
    CHECK(!cmd.enabledOption);
    // DM (guild 無し・member 無し)
    Json dm = interaction("discutere-queue", Json::array());
    dm.erase("guild_id");
    dm.erase("channel_id");
    dm.erase("member");
    dm["user"] = Json{{"id", "u9"}};
    cmd = inbound_slash_command_from_json(dm);
    CHECK_EQ(cmd.guildId, std::string("dm"));
    CHECK_EQ(cmd.channelId, std::string("default"));
    CHECK_EQ(cmd.userId.value_or(""), std::string("u9"));
    CHECK_THROWS(inbound_slash_command_from_json(Json::array()));
}

TEST_CASE("handle_slash_interaction: 返信・admin 境界・壊れた入力・例外は internal error") {
    w4q::Env env;
    RecordingReplier rep;
    auto d = deps_with(env, {});
    CHECK(handle_slash_interaction(interaction("discutere-status", Json::array()), d, rep));
    CHECK_EQ(rep.replies.size(), std::size_t{1});
    CHECK(str::contains(rep.replies[0].content, "disabled"));
    // 壊れた interaction は返信しない
    CHECK(!handle_slash_interaction(Json{{"id", "x"}}, d, rep));
    CHECK(!handle_slash_interaction(Json("str"), d, rep));
    CHECK_EQ(rep.replies.size(), std::size_t{1});
    // route が例外 (KG を開けない) → internal error (例外の中身は返信に出さない)
    d.openCore = []() -> std::unique_ptr<core::Core> { throw std::runtime_error("/secret/path/kg.kuzu"); };
    CHECK(handle_slash_interaction(interaction("propose", Json::array()), d, rep));
    CHECK_EQ(rep.replies.back().content, std::string("⚠️ internal error"));
    CHECK(rep.replies.back().ephemeral);
    // 返信本文の形 (type 4 / ephemeral flag / メンションを解決しない)
    CHECK_EQ(json_stringify(slash_reply_body({"x", true})),
             std::string(R"({"type":4,"data":{"content":"x","flags":64,"allowed_mentions":{"parse":[]}}})"));
    CHECK_EQ(json_stringify(slash_reply_body({"y", false})), std::string(R"({"type":4,"data":{"content":"y","allowed_mentions":{"parse":[]}}})"));
}

TEST_CASE("Discatier コマンドは discord-session に記録 (同じ channel は同じ session)") {
    w4q::Env env;
    const auto d = deps_with(env, {});
    auto r = route_slash_command(slash("propose", std::string("u1"), "新しい仮説"), d);
    CHECK(!r.ephemeral);
    CHECK(str::starts_with(r.content, "✅ hypothesis "));
    route_slash_command(slash("integrate", std::string("u1")), d);
    CHECK_EQ(count(env, "SELECT COUNT(*) AS n FROM sessions WHERE title = 'discord-session:g1:ch1' AND scene = 'discord:g1/ch1'"),
             std::int64_t{1});
}

// ─── 平文 / フォーラムの取り込み ────────────────────────────────────────

TEST_CASE("route_inbound_message: 許可チャンネル (親継承)・空・slash は取り込まない・seed は分類器の結果") {
    w4q::Env env;
    auto d = deps_with(env, {});
    d.discussionChannelIds = {"chA"};
    std::vector<DiscordAutoDiscussionInput> seen;
    d.classifyInboundMessage = [&seen](const DiscordAutoDiscussionInput& in) {
        seen.push_back(in);
        return StarterResult{in.content == "議論しよう"};
    };
    CHECK(!route_inbound_message(message("chA", "   "), "g1", d).ingested);
    CHECK(!route_inbound_message(message("chA", "  /propose x"), "g1", d).ingested);
    auto r = route_inbound_message(message("chA", "  議論しよう  "), "g1", d);
    CHECK(r.ingested);
    CHECK(r.seed);
    CHECK(seen.empty());  // seed を呼ぶまで分類しない
    CHECK(r.seed());
    CHECK_EQ(seen[0].content, std::string("議論しよう"));
    CHECK_EQ(seen[0].authorId, std::string("u1"));
    // スレッドは親が許可なら継承 (session は実際の発言先)
    r = route_inbound_message(message("thread9", "雑談"), "g1", d, std::string("chA"));
    CHECK(r.ingested);
    CHECK(!r.seed());
    CHECK_EQ(count(env, "SELECT COUNT(*) AS n FROM sessions WHERE scene = 'discord:g1/thread9'"), std::int64_t{1});
    // 分類器の例外は false (落ちない)
    d.classifyInboundMessage = [](const DiscordAutoDiscussionInput&) -> StarterResult { throw std::runtime_error("boom"); };
    r = route_inbound_message(message("chA", "x"), "g1", d);
    CHECK(!r.seed());
    // 分類器未設定なら seed 無し
    d.classifyInboundMessage = nullptr;
    CHECK(!route_inbound_message(message("chA", "y"), "g1", d).seed);
}

TEST_CASE("handle_inbound_message_json: 正規化できない payload は取り込まない") {
    w4q::Env env;
    auto d = deps_with(env, {});
    d.discussionChannelIds = {"chA"};
    CHECK(!handle_inbound_message_json(Json::object(), "g1", d).ingested);
    CHECK(!handle_inbound_message_json(Json("x"), "g1", d).ingested);
    const Json ok{{"id", "m1"}, {"channel_id", "chA"}, {"content", "hello"}, {"timestamp", "t"}, {"author", Json{{"id", "u1"}, {"username", "a"}}}};
    CHECK(handle_inbound_message_json(ok, "g1", d).ingested);
    Json big = ok;
    big["content"] = std::string(1u << 20, 'x');
    CHECK(handle_inbound_message_json(big, "g1", d).ingested);
}

TEST_CASE("route_forum_post: starter だけ seed、open な議論があれば二重起動しない、許可リストに依らない") {
    w4q::Env env;
    auto d = deps_with(env, {});
    d.classifyInboundMessage = [](const DiscordAutoDiscussionInput& in) { return StarterResult{in.forumTitle.has_value()}; };
    ForumPostOptions starter;
    starter.isStarter = true;
    starter.forumTitle = "題";
    starter.direction = "improvement";
    starter.forumTags = std::vector<std::string>{"改善"};
    auto r = route_forum_post(message("T1", "本文"), "G1", d, starter);
    CHECK(r.ingested);
    CHECK(r.seed && r.seed());
    ForumPostOptions reply;
    CHECK(!route_forum_post(message("T1", "後続"), "G1", d, reply).seed);
    // open な議論 (scene=discord:G1/T1) が立っていれば starter でも seed しない
    env.kg_exec(Json::array({"INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('g1', 'ws', 't', 'open', 1, 1)",
                             "INSERT INTO sessions (id, workspace_id, title, started_at, scene, created_at, updated_at) VALUES ('sg1', 'ws', "
                             "'discussion-of-gap:g1', 1, 'discord:G1/T1', 1, 1)"}));
    CHECK(!route_forum_post(message("T1", "再送"), "G1", d, starter).seed);
    // 終わった議論 (converged) なら立てられる
    env.kg_exec(Json::array({"UPDATE design_gaps SET status = 'converged' WHERE id = 'g1'"}));
    CHECK(route_forum_post(message("T1", "再開"), "G1", d, starter).seed);
    CHECK(!route_forum_post(message("T1", "/slash"), "G1", d, starter).ingested);
}

TEST_CASE("resolve_seed_in_background: 結果を渡す・満杯なら false で縮退") {
    std::vector<bool> got;
    resolve_seed_in_background([] { return true; }, [&got](bool s) { got.push_back(s); },
                               [](std::function<void()> fn) {
                                   fn();
                                   return true;
                               });
    resolve_seed_in_background([] { return true; }, [&got](bool s) { got.push_back(s); }, [](std::function<void()>) { return false; });
    resolve_seed_in_background(SeedTask{}, [&got](bool s) { got.push_back(s); });
    CHECK_EQ(got.size(), std::size_t{2});
    CHECK(got[0]);
    CHECK(!got[1]);
}

TEST_CASE("fire-and-forget: backup / persona-generate は背景で 1 回、満杯なら開始できない旨") {
    w4q::Env env;
    auto d = deps_with(env, {"admin1"});
    std::atomic<int> calls{0};
    std::optional<std::string> source;
    d.triggerBackup = [&calls] {
        ++calls;
        backup::BackupResult r;
        r.ok = true;
        r.bucket = "b";
        r.key = "k";
        return r;
    };
    d.triggerPersonaGenerate = [&](const std::optional<std::string>& s) {
        ++calls;
        source = s;
        return PersonaGenerateResult{2, 5};
    };
    CHECK(str::contains(route_slash_command(slash("discutere-backup"), d).content, "開始しました"));
    CHECK(str::contains(route_slash_command(slash("persona-generate", std::string("admin1"), "  steam "), d).content, "(source=steam)"));
    CHECK_EQ(source.value_or(""), std::string("steam"));
    CHECK_EQ(calls.load(), 2);
    d.submitBackground = [](std::function<void()>) { return false; };
    CHECK(str::contains(route_slash_command(slash("discutere-backup"), d).content, "開始できませんでした"));
    CHECK_EQ(calls.load(), 2);
    // trigger の例外は背景で握りつぶす
    d.submitBackground = [](std::function<void()> fn) {
        fn();
        return true;
    };
    d.triggerBackup = []() -> backup::BackupResult { throw std::runtime_error("s3 down"); };
    CHECK(str::contains(route_slash_command(slash("discutere-backup"), d).content, "開始しました"));
    // economy-graph
    d.triggerEconomyAnalysis = [](const std::string& t) { return EconomyAnalysisStarted{"slug-" + t, 4321}; };
    CHECK(str::contains(route_slash_command(slash("economy-graph", std::nullopt, " Slay "), d).content,
                        "http://localhost:4321/ludus/economy-graph/slug-Slay"));
    // queue の例外は文言にする
    d.buildQueueText = []() -> std::string { throw std::runtime_error("db"); };
    CHECK_EQ(route_slash_command(slash("discutere-queue"), d).content, std::string("⚠️ queue 取得失敗: db"));
}

TEST_CASE("整形: truncate_text (連続改行・UTF-16 の長さ) と ja_locale_string") {
    CHECK_EQ(truncate_text("a\n\n\nb\nc", 100), std::string("a\nb\nc"));
    CHECK_EQ(truncate_text("abcdef", 6), std::string("abcdef"));
    CHECK_EQ(truncate_text("abcdefg", 6), std::string("abcde…"));
    CHECK_EQ(truncate_text("あいう😀", 4), std::string("あいう…"));
    CHECK_EQ(truncate_text("", 3), std::string(""));
    w4q::Env env;  // TZ=UTC
    CHECK_EQ(ja_locale_string(1700000000000.0), std::string("2023/11/14 22:13:20"));
    CHECK_EQ(ja_locale_string(3000), std::string("1970/1/1 0:00:03"));
    CHECK_EQ(ja_locale_string(1e300), std::string("Invalid Date"));
}

DI_TEST_MAIN()
