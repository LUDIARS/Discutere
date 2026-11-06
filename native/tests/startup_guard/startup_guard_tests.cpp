// w2-J: src/startup-guard.ts の移植 (tests/startup-guard/run.ts) + 秘密値をメッセージに出さないこと。
#include "check.hpp"

#include "base/strings.hpp"
#include "startup_guard.hpp"

using namespace di;

namespace {
config::DiscutereConfig make(const std::string& env, std::optional<std::string> token, std::vector<std::string> admins = {},
                             std::string backend = "claude-cli") {
    config::DiscutereConfig c;
    c.nodeEnv = env;
    c.discord.botToken = std::move(token);
    c.discord.adminIds = std::move(admins);
    c.llm.backend = std::move(backend);
    return c;
}
} // namespace

TEST_CASE("startup guard (run.ts)") {
    std::vector<std::string> warns;
    auto w = [&](const std::string& m) { warns.push_back(m); };
    assert_startup_health(make("development", std::string("")), w);
    assert_startup_health(make("test", std::nullopt), w);
    CHECK(warns.empty());
    CHECK_THROWS(assert_startup_health(make("production", std::string("")), w));
    CHECK_THROWS(assert_startup_health(make("production", std::nullopt), w));
    try {
        assert_startup_health(make("production", std::nullopt), w);
    } catch (const StartupHealthError& e) {
        CHECK(str::contains(e.what(), "DISCUTERE_DISCORD_BOT_TOKEN"));
    }
    assert_startup_health(make("production", std::string("fake-token-value"), {}), w);
    CHECK(warns.size() == 1 && str::contains(warns[0], "adminIds") && str::contains(warns[0], "全 deny"));
    warns.clear();
    assert_startup_health(make("production", std::string("fake-token-value"), {"a"}, "mock"), w);
    CHECK(warns.size() == 1 && str::contains(warns[0], "mock"));
    warns.clear();
    assert_startup_health(make("production", std::string("fake-token-value"), {"a"}), w);
    CHECK(warns.empty());
    // "Production" (大文字) は production ではない (TS と同じ厳密一致)
    assert_startup_health(make("Production", std::nullopt), w);
}

DI_TEST_MAIN()
