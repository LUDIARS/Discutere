// worker-pool/spawner の移植テスト: 引数配列・env の扱い・切り離した起動・kill (偽の lictor)。
#include "check.hpp"

#include <chrono>
#include <filesystem>
#include <thread>

#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "persona_engine/worker_pool/spawner.hpp"
#ifndef _WIN32
#include <csignal>
#include <sys/stat.h>
#endif

using namespace di;
using namespace di::persona_engine::worker_pool;

namespace {
SpawnWorkerArgs sample_args(const std::string& cwd) {
    SpawnWorkerArgs a;
    a.worker = {"con-opus", "否定派", "claude", "claude-opus-5"};
    a.promptPath = "/p/prompt.md";
    a.cwd = cwd;
    a.cfg.callbackBaseUrl = "http://127.0.0.1:3110";
    a.cfg.injectDelayMs = 1500;
    return a;
}
} // namespace

TEST_CASE("lictor args: claude gets acceptEdits, codex does not") {
    std::vector<std::string> c = {"claude", "--model", "claude-opus-5", "--permission-mode", "acceptEdits"};
    CHECK(lictor_args({"a", "r", "claude", "claude-opus-5"}) == c);
    std::vector<std::string> x = {"codex", "--model", "gpt-5.6-sol"};
    CHECK(lictor_args({"a", "r", "codex", "gpt-5.6-sol"}) == x);
    std::vector<std::string> u = {"claude", "--model", "m"};
    CHECK(lictor_args({"a", "r", "other", "m"}) == u);  // 未知 provider は claude bin (flag なし)
}

TEST_CASE("worker env: adds worker values and strips parent Lictor/Concordia wiring") {
    std::map<std::string, std::string> parent = {{"PATH", "/bin"},           {"CONCORDIA_HOOK", "1"}, {"LICTOR_PORT", "9"},
                                                 {"LICTOR_PID", "1"},        {"LICTOR_SESSION_ID", "s"},
                                                 {"CONCORDIA_SESSION_ID", "c"}, {"LICTOR_TRANSCRIPT_FILE", "/t"}};
    auto env = build_worker_environment(parent, sample_args("/w"));
    CHECK_EQ(env.at("PATH"), std::string("/bin"));
    CHECK_EQ(env.at("CONCORDIA_DELEGATION_PROMPT_FILE"), std::string("/p/prompt.md"));
    CHECK_EQ(env.at("LICTOR_DELEGATION_INJECT_DELAY_MS"), std::string("1500"));
    CHECK_EQ(env.at("LICTOR_DISABLE_CONCORDIA"), std::string("1"));
    CHECK_EQ(env.at("DI_WORKER_ID"), std::string("con-opus"));
    CHECK_EQ(env.at("DI_CALLBACK_URL"), std::string("http://127.0.0.1:3110"));
    CHECK_EQ(env.at("LICTOR_PIN_TRANSCRIPT"), std::string("1"));
    for (const char* k : {"CONCORDIA_HOOK", "LICTOR_PORT", "LICTOR_PID", "LICTOR_SESSION_ID", "CONCORDIA_SESSION_ID", "LICTOR_TRANSCRIPT_FILE"})
        CHECK(env.find(k) == env.end());
}

TEST_CASE("resolve_git_bash: explicit existing path wins, missing explicit falls back") {
    const auto dir = std::filesystem::temp_directory_path() / ("gb-" + crypto::uuid_v4());
    fs::ensure_dir(fs::from_path(dir));
    const std::string bash = fs::from_path(dir / "bash.exe");
    fs::write_file_atomic(bash, "x");
    CHECK(resolve_git_bash(bash) == std::optional<std::string>(bash));
#ifndef _WIN32
    CHECK(!resolve_git_bash(std::string("/nonexistent/bash.exe")));
#endif
}

#ifndef _WIN32
TEST_CASE("spawn_worker launches fake lictor detached with argv/env/cwd and kill_worker stops the group") {
    const auto dir = std::filesystem::temp_directory_path() / ("di-fake-lictor-" + crypto::uuid_v4());
    const auto bin = dir / "bin";
    const auto home = dir / "home";
    fs::ensure_dir(fs::from_path(bin));
    fs::ensure_dir(fs::from_path(home));
    const std::string lictor = fs::from_path(bin / "lictor");
    // 偽の lictor: 引数と env と cwd を書き出して常駐 (sleep) する。
    fs::write_file_atomic(lictor, "#!/bin/sh\nprintf '%s|' \"$@\" > args.txt\npwd > cwd.txt\n"
                                  "printf '%s %s %s' \"$DI_WORKER_ID\" \"${LICTOR_PORT:-none}\" \"$LICTOR_DISABLE_CONCORDIA\" > env.txt\n"
                                  "exec sleep 30\n");
    ::chmod(lictor.c_str(), 0755);
    const auto old_path = env::get("PATH").value_or("/usr/bin:/bin");
    env::set_for_test("PATH", fs::from_path(bin) + ":" + old_path);
    env::set_for_test("LICTOR_PORT", std::string_view("4321"));
    auto spawned = spawn_worker(sample_args(fs::from_path(home)));
    env::set_for_test("PATH", old_path);
    env::set_for_test("LICTOR_PORT", std::nullopt);
    CHECK(spawned.pid.has_value());
    const std::string env_file = fs::from_path(home / "env.txt");
    for (int i = 0; i < 100 && !(fs::exists(env_file) && fs::file_size(env_file).value_or(0) > 0); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(*fs::read_file(fs::from_path(home / "args.txt")), std::string("claude|--model|claude-opus-5|--permission-mode|acceptEdits|"));
    CHECK_EQ(*fs::read_file(env_file), std::string("con-opus none 1"));
    CHECK(fs::read_file(fs::from_path(home / "cwd.txt"))->find("home") != std::string::npos);
    CHECK_EQ(::kill(static_cast<pid_t>(*spawned.pid), 0), 0);  // 生きている
    kill_worker(spawned.pid);
    bool gone = false;
    for (int i = 0; i < 100 && !gone; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        gone = ::kill(static_cast<pid_t>(*spawned.pid), 0) != 0;
    }
    CHECK(gone);
}

TEST_CASE("spawn_detached refuses missing executable and empty argv") {
    std::string error;
    CHECK(!spawn_detached({{"definitely-not-a-binary-xyz"}, env::snapshot(), ""}, &error));
    CHECK(error.find("executable not found") != std::string::npos);
    CHECK(!spawn_detached({{}, {}, ""}, &error));
    kill_worker(std::nullopt, std::nullopt);  // 何もしない
}
#endif

DI_TEST_MAIN()
