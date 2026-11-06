// w4q_command_router のテスト (1/4)。準備は w4q_command_router_support.hpp。
#include "w4q_command_router_support.hpp"

using namespace w4q_command_router_support;

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

DI_TEST_MAIN()
