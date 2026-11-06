#include "base/sort_by_index.hpp"
#include "persona_engine/worker_pool/spawner.hpp"

#include <thread>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/log.hpp"
#include "base/process.hpp"

#ifdef _WIN32
#include <windows.h>
#include <algorithm>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace di::persona_engine::worker_pool {

std::optional<std::string> resolve_git_bash(const std::optional<std::string>& explicit_path) {
    if (explicit_path && !explicit_path->empty() && fs::exists(*explicit_path)) return explicit_path;
    std::string user = env::get("USERNAME").value_or(env::get("USER").value_or(""));
    const std::vector<std::string> cands = {
        "C:\\Program Files\\Git\\bin\\bash.exe",
        "C:\\Program Files (x86)\\Git\\bin\\bash.exe",
        "C:\\Users\\" + user + "\\AppData\\Local\\Atlassian\\SourceTree\\git_local\\bin\\bash.exe",
    };
    for (const auto& c : cands)
        if (fs::exists(c)) return c;
    return std::nullopt;
}

std::vector<std::string> lictor_args(const WorkerConfig& worker) {
    const std::string bin = worker.provider == "codex" ? "codex" : "claude";
    // claude は edit-mode (acceptEdits) で起動する (codex は claude 専用 flag なので付けない)。
    if (worker.provider == "claude") return {bin, "--model", worker.model, "--permission-mode", "acceptEdits"};
    return {bin, "--model", worker.model};
}

std::map<std::string, std::string> build_worker_environment(const std::map<std::string, std::string>& parent,
                                                            const SpawnWorkerArgs& args) {
    auto env = parent;
    env["CONCORDIA_DELEGATION_PROMPT_FILE"] = args.promptPath;
    env["LICTOR_DELEGATION_INJECT_DELAY_MS"] = std::to_string(args.cfg.injectDelayMs);
    env["LICTOR_DISABLE_CONCORDIA"] = "1";
    env["DI_WORKER_ID"] = args.worker.id;
    env["DI_CALLBACK_URL"] = args.cfg.callbackBaseUrl;
    // Lictor に session-id 固定 + transcript path 公開を依頼する (send.mjs が usage を回収する)。
    env["LICTOR_PIN_TRANSCRIPT"] = "1";
    if (auto gb = resolve_git_bash(args.cfg.gitBashPath)) env["CLAUDE_CODE_GIT_BASH_PATH"] = *gb;
    // 親 (この Discutere を起動した Lictor/Concordia) のラップ情報を子に持ち込まない。
    for (const char* k : {"CONCORDIA_HOOK", "LICTOR_PORT", "LICTOR_PID", "LICTOR_SESSION_ID", "CONCORDIA_SESSION_ID",
                          "LICTOR_TRANSCRIPT_FILE"})
        env.erase(k);
    return env;
}

#ifndef _WIN32

std::optional<std::int64_t> spawn_detached(const DetachedSpawn& spec, std::string* error) {
    if (spec.argv.empty()) {
        if (error) *error = "empty argv";
        return std::nullopt;
    }
    const auto exe = proc::find_executable(spec.argv[0], spec.env);
    if (!exe) {
        if (error) *error = "executable not found: " + spec.argv[0];
        return std::nullopt;
    }
    std::vector<std::string> env_strings;
    for (const auto& [k, v] : spec.env) env_strings.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& s : env_strings) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<std::string> args = spec.argv;
    std::vector<char*> argvp;
    for (auto& s : args) argvp.push_back(s.data());
    argvp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // stdio: "ignore"
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    if (!spec.cwd.empty()) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
        posix_spawn_file_actions_addchdir_np(&actions, spec.cwd.c_str());
#else
        posix_spawn_file_actions_destroy(&actions);
        if (error) *error = "cwd is not supported on this platform";
        return std::nullopt;
#endif
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t empty_mask, default_signals;
    sigemptyset(&empty_mask);
    sigemptyset(&default_signals);
    sigaddset(&default_signals, SIGPIPE);
    posix_spawnattr_setsigmask(&attr, &empty_mask);
    posix_spawnattr_setsigdefault(&attr, &default_signals);
    // detached: true (新しいセッション = プロセスグループの長。kill(-pid) で木ごと止められる)。
#ifdef POSIX_SPAWN_SETSID
    short flags = POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#else
    posix_spawnattr_setpgroup(&attr, 0);
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#endif
    posix_spawnattr_setflags(&attr, flags);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe->c_str(), &actions, &attr, argvp.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        if (error) *error = std::string("spawn failed: ") + std::strerror(rc);
        return std::nullopt;
    }
    // child.unref(): 待たずに戻る。終了したら背景スレッドで回収する (ゾンビを残さない)。
    std::thread([pid] {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
    return static_cast<std::int64_t>(pid);
}

#else  // _WIN32

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
} // namespace

std::optional<std::int64_t> spawn_detached(const DetachedSpawn& spec, std::string* error) {
    if (spec.argv.empty()) {
        if (error) *error = "empty argv";
        return std::nullopt;
    }
    // 実行ファイル (.exe) だけを起動する。.cmd / .bat は cmd.exe を経由するので起動しない。
    const auto exe = proc::find_executable(spec.argv[0], spec.env);
    if (!exe) {
        if (error) *error = "executable not found: " + spec.argv[0];
        return std::nullopt;
    }
    std::string cmdline;
    for (std::size_t i = 0; i < spec.argv.size(); ++i) {
        if (i) cmdline.push_back(' ');
        cmdline += proc::quote_windows_arg(i == 0 ? *exe : spec.argv[i]);
    }
    std::vector<std::pair<std::wstring, std::wstring>> pairs;
    for (const auto& [k, v] : spec.env) pairs.emplace_back(widen(k), widen(v));
    di::stable_sort_by_index(pairs, [](const auto& a, const auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    std::wstring env_block;
    for (const auto& [k, v] : pairs) {
        env_block += k + L"=" + v;
        env_block.push_back(L'\0');
    }
    env_block.push_back(L'\0');
    std::wstring wcmd = widen(cmdline);
    std::wstring wexe = widen(*exe);
    std::wstring wcwd = widen(spec.cwd);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // detached + stdio ignore + windowsHide。ハンドルは継承しない。
    const BOOL ok = CreateProcessW(wexe.c_str(), wcmd.data(), nullptr, nullptr, FALSE,
                                   CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
                                   env_block.data(), spec.cwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    if (!ok) {
        if (error) *error = "CreateProcess failed: " + std::to_string(GetLastError());
        return std::nullopt;
    }
    const auto pid = static_cast<std::int64_t>(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pid;
}

#endif

SpawnedWorker spawn_worker(const SpawnWorkerArgs& args) {
    DetachedSpawn spec;
    spec.argv.push_back("lictor");
    for (auto& a : lictor_args(args.worker)) spec.argv.push_back(std::move(a));
    spec.env = build_worker_environment(env::snapshot(), args);
    spec.cwd = args.cwd;
    // Windows: TS は lictor (.cmd shim) を cmd.exe /c で起動していたが、シェルを経由しないため
    // lictor の実行ファイル (lictor.exe) を PATH から直接起動する。見つからなければ起動しない。
    std::string error;
    auto pid = spawn_detached(spec, &error);
    if (!pid) log::warn("[worker-pool] spawn failed (" + args.worker.id + "): " + error);
    return {pid};
}

void kill_worker(std::optional<std::int64_t> cmd_pid, std::optional<std::int64_t> lictor_pid) {
    std::vector<std::int64_t> pids;
    for (const auto& p : {lictor_pid, cmd_pid})
        if (p && *p > 0) pids.push_back(*p);
    for (const auto pid : pids) {
#ifdef _WIN32
        DetachedSpawn spec;
        spec.argv = {"taskkill", "/PID", std::to_string(pid), "/T", "/F"};
        spec.env = env::snapshot();
        spawn_detached(spec);
#else
        ::kill(-static_cast<pid_t>(pid), SIGTERM);  // best-effort (プロセスグループごと)
#endif
    }
}

} // namespace di::persona_engine::worker_pool
