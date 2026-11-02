// src/flow/github-cli.ts の移植の確認。gh の代わりにテスト内で作る偽のスクリプトを引数配列で起動する。
#include "check.hpp"

#include <filesystem>

#include "base/crypto.hpp"
#include "base/fsutil.hpp"
#include "flow/github_cli.hpp"

using namespace di;
using namespace di::flow;

namespace {
struct FakeGh {
    std::string dir, path;
    explicit FakeGh(const std::string& body) {
        dir = fs::from_path(std::filesystem::temp_directory_path() / ("w1e-gh-" + crypto::uuid_v4()));
        fs::ensure_dir(dir);
        path = fs::join(dir, "gh");
        fs::write_file_atomic(path, "#!/bin/sh\n" + body);
        std::filesystem::permissions(fs::to_path(path), std::filesystem::perms::owner_all);
    }
    ~FakeGh() {
        std::error_code ec;
        std::filesystem::remove_all(fs::to_path(dir), ec);
    }
};
} // namespace

TEST_CASE("strip ANSI and redact tokens") {
    CHECK_EQ(strip_ansi("\x1b[1;32m✓\x1b[0m Logged in \x1b[x"), std::string("✓ Logged in \x1b[x"));
    const std::string tok = "ghp_" + std::string(36, 'A');
    CHECK_EQ(redact_secrets("Token: " + tok + " ok"), std::string("Token: [redacted-token] ok"));
    CHECK_EQ(redact_secrets("github_pat_" + std::string(22, 'b') + "_x"), std::string("[redacted-token]"));
    CHECK_EQ(redact_secrets("ghp_short"), std::string("ghp_short"));
    CHECK_EQ(redact_secrets("xghp_" + std::string(30, 'a')), "xghp_" + std::string(30, 'a'));  // 語の途中は伏せない (\b)
}

#ifndef _WIN32
TEST_CASE("auth status and token through a fake gh") {
    const std::string tok = "gho_" + std::string(30, 'z');
    FakeGh ok("if [ \"$2\" = token ]; then echo '" + tok + "'; exit 0; fi\nprintf '\\033[32mLogged in\\033[0m with " + tok + "\\n' 1>&2\nexit 0\n");
    const auto s = get_github_cli_auth_status(ok.path);
    CHECK(s.available);
    CHECK(s.authenticated);
    CHECK_EQ(s.output, std::string("Logged in with [redacted-token]"));
    CHECK_EQ(*get_github_cli_token(ok.path), tok);

    FakeGh unauth("echo 'You are not logged into any GitHub hosts.' 1>&2\nexit 1\n");
    const auto u = get_github_cli_auth_status(unauth.path);
    CHECK(u.available);
    CHECK(!u.authenticated);
    CHECK(!get_github_cli_token(unauth.path).has_value());

    const auto missing = get_github_cli_auth_status("/nonexistent/gh-missing");
    CHECK(!missing.available);
    CHECK(!missing.authenticated);
    CHECK(missing.output.find("ENOENT") != std::string::npos);

    FakeGh slow("sleep 10\n");
    const auto r = run_gh({"auth", "status"}, 200, slow.path);
    CHECK_EQ(r.exitCode, 1);
    CHECK(r.error.has_value());

    FakeGh flood("head -c 3000000 /dev/zero | tr '\\0' 'a'\n");
    const auto f = run_gh({"auth", "token"}, 5000, flood.path);
    CHECK_EQ(f.exitCode, 1);
    CHECK(!get_github_cli_token(flood.path).has_value());
}
#endif

DI_TEST_MAIN()
