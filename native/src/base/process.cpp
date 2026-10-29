#include "base/process.hpp"

#include <algorithm>
#include <mutex>

#include <lorica/input_budget.hpp>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"

#ifdef _WIN32
#include <windows.h>
#include <thread>
#else
#include <cerrno>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace di::proc {

std::string quote_windows_arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            out.push_back(c);
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

std::optional<std::string> find_executable(const std::string& name, const std::map<std::string, std::string>& env) {
    if (name.empty()) return std::nullopt;
#ifdef _WIN32
    const bool has_dir = name.find_first_of("/\\") != std::string::npos;
    const std::string ext = fs::extension_lower(name);
    auto candidates = [&](const std::string& base) {
        std::vector<std::string> c;
        if (ext == ".exe" || ext == ".com") c.push_back(base);
        else c.push_back(base + ".exe");
        return c;
    };
    if (has_dir) {
        for (const auto& c : candidates(name))
            if (fs::is_file(c)) return c;
        return std::nullopt;
    }
    std::string path;
    for (const auto& [k, v] : env)
        if (str::iequals_ascii(k, "PATH")) path = v;
    for (const auto& dir : str::split(path, ";")) {
        if (dir.empty()) continue;
        for (const auto& c : candidates(fs::join(dir, name)))
            if (fs::is_file(c)) return c;
    }
    return std::nullopt;
#else
    if (name.find('/') != std::string::npos) {
        if (access(name.c_str(), X_OK) == 0) return name;
        return std::nullopt;
    }
    std::string path = "/usr/local/bin:/usr/bin:/bin";
    if (auto it = env.find("PATH"); it != env.end()) path = it->second;
    for (const auto& dir : str::split(path, ":")) {
        const std::string candidate = (dir.empty() ? std::string(".") : dir) + "/" + name;
        struct stat st {};
        if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return std::nullopt;
#endif
}

#ifndef _WIN32

namespace {

void ignore_sigpipe_once() {
    static std::once_flag once;
    std::call_once(once, [] { std::signal(SIGPIPE, SIG_IGN); });
}

bool make_pipe(int fds[2]) {
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
}

void set_nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

} // namespace

Result run(const Options& options) {
    Result result;
    const auto started_at = time::steady_ms();
    if (options.argv.empty()) {
        result.start_error = "empty argv";
        return result;
    }
    ignore_sigpipe_once();
    const auto env_map = options.env ? *options.env : env::snapshot();
    const auto exe = find_executable(options.argv[0], env_map);
    if (!exe) {
        result.start_error = "executable not found: " + options.argv[0];
        return result;
    }
    std::vector<std::string> env_strings;
    env_strings.reserve(env_map.size());
    for (const auto& [k, v] : env_map) env_strings.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& s : env_strings) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<std::string> args = options.argv;
    std::vector<char*> argvp;
    for (auto& s : args) argvp.push_back(s.data());
    argvp.push_back(nullptr);

    int in_pipe[2], out_pipe[2], err_pipe[2];
    if (!make_pipe(in_pipe)) {
        result.start_error = "pipe failed";
        return result;
    }
    if (!make_pipe(out_pipe)) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        result.start_error = "pipe failed";
        return result;
    }
    if (!make_pipe(err_pipe)) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        result.start_error = "pipe failed";
        return result;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);
    if (!options.cwd.empty()) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
        posix_spawn_file_actions_addchdir_np(&actions, options.cwd.c_str());
#else
        result.start_error = "cwd is not supported on this platform";
        posix_spawn_file_actions_destroy(&actions);
        return result;
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
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe->c_str(), &actions, &attr, argvp.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (rc != 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(err_pipe[0]);
        result.start_error = std::string("spawn failed: ") + std::strerror(rc);
        return result;
    }
    result.started = true;
    int in_fd = in_pipe[1];
    int out_fd = out_pipe[0];
    int err_fd = err_pipe[0];
    set_nonblocking(in_fd);
    set_nonblocking(out_fd);
    set_nonblocking(err_fd);

    lorica::input_budget out_budget({options.max_stdout, options.max_stdout, options.max_stdout, 1});
    lorica::input_budget err_budget({options.max_stderr, options.max_stderr, options.max_stderr, 1});
    std::size_t stdin_offset = 0;
    if (options.stdin_data.empty()) {
        close(in_fd);
        in_fd = -1;
    }
    bool killed = false;
    auto kill_group = [&] {
        if (!killed) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            killed = true;
        }
    };
    const auto deadline = started_at + options.timeout.count();
    char buf[16384];
    while (out_fd >= 0 || err_fd >= 0) {
        if (!killed) {
            if (time::steady_ms() >= deadline) {
                result.timed_out = true;
                kill_group();
            } else if (options.should_cancel && options.should_cancel()) {
                result.cancelled = true;
                kill_group();
            }
        }
        pollfd fds[3];
        nfds_t n = 0;
        int idx_in = -1, idx_out = -1, idx_err = -1;
        if (in_fd >= 0) { idx_in = static_cast<int>(n); fds[n++] = {in_fd, POLLOUT, 0}; }
        if (out_fd >= 0) { idx_out = static_cast<int>(n); fds[n++] = {out_fd, POLLIN, 0}; }
        if (err_fd >= 0) { idx_err = static_cast<int>(n); fds[n++] = {err_fd, POLLIN, 0}; }
        const int pr = poll(fds, n, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (idx_in >= 0 && (fds[idx_in].revents & (POLLOUT | POLLERR | POLLHUP))) {
            const auto remaining = options.stdin_data.size() - stdin_offset;
            const ssize_t w = write(in_fd, options.stdin_data.data() + stdin_offset, std::min<std::size_t>(remaining, 65536));
            if (w > 0) stdin_offset += static_cast<std::size_t>(w);
            if (w < 0 && errno != EAGAIN && errno != EINTR) stdin_offset = options.stdin_data.size();  // EPIPE: 子が読まずに終わった
            if (stdin_offset >= options.stdin_data.size()) {
                close(in_fd);
                in_fd = -1;
            }
        }
        auto drain = [&](int& fd, int idx, std::string& sink, lorica::input_budget& budget) {
            if (fd < 0 || idx < 0 || !(fds[idx].revents & (POLLIN | POLLHUP | POLLERR))) return;
            while (true) {
                const ssize_t r = read(fd, buf, sizeof buf);
                if (r > 0) {
                    if (!budget.consume(static_cast<std::size_t>(r), 0, 0)) {
                        result.output_exceeded = true;
                        kill_group();
                        close(fd);
                        fd = -1;
                        return;
                    }
                    sink.append(buf, static_cast<std::size_t>(r));
                    continue;
                }
                if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                    close(fd);
                    fd = -1;
                }
                return;
            }
        };
        drain(out_fd, idx_out, result.out, out_budget);
        drain(err_fd, idx_err, result.err, err_budget);
    }
    if (in_fd >= 0) close(in_fd);
    if (out_fd >= 0) close(out_fd);
    if (err_fd >= 0) close(err_fd);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!killed && WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    result.duration_ms = time::steady_ms() - started_at;
    return result;
}

#else  // _WIN32

namespace {

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset(HANDLE v = nullptr) {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = v;
    }
};

} // namespace

Result run(const Options& options) {
    Result result;
    const auto started_at = time::steady_ms();
    if (options.argv.empty()) {
        result.start_error = "empty argv";
        return result;
    }
    const auto env_map = options.env ? *options.env : env::snapshot();
    const auto exe = find_executable(options.argv[0], env_map);
    if (!exe) {
        result.start_error = "executable not found: " + options.argv[0];
        return result;
    }
    std::string cmdline;
    for (std::size_t i = 0; i < options.argv.size(); ++i) {
        if (i) cmdline.push_back(' ');
        cmdline += quote_windows_arg(i == 0 ? *exe : options.argv[i]);
    }
    // 環境ブロック: 名前の大文字小文字を無視した昇順 (CreateProcess の要件)。
    std::vector<std::pair<std::wstring, std::wstring>> pairs;
    for (const auto& [k, v] : env_map) pairs.emplace_back(widen(k), widen(v));
    std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    std::wstring env_block;
    for (const auto& [k, v] : pairs) {
        env_block += k + L"=" + v;
        env_block.push_back(L'\0');
    }
    env_block.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0)) {
        result.start_error = "pipe failed";
        return result;
    }
    Handle child_in(in_r), parent_in(in_w), parent_out(out_r), child_out(out_w), parent_err(err_r), child_err(err_w);
    SetHandleInformation(parent_in.h, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parent_out.h, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parent_err.h, HANDLE_FLAG_INHERIT, 0);

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    HANDLE inherit[3] = {child_in.h, child_out.h, child_err.h};
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = child_in.h;
    si.StartupInfo.hStdOutput = child_out.h;
    si.StartupInfo.hStdError = child_err.h;
    si.lpAttributeList = attrs;

    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (job.h) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &info, sizeof info);
    }

    std::wstring wcmd = widen(cmdline);
    std::wstring wexe = widen(*exe);
    std::wstring wcwd = widen(options.cwd);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(wexe.c_str(), wcmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
                                       CREATE_SUSPENDED,
                                   env_block.data(), options.cwd.empty() ? nullptr : wcwd.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    child_in.reset();
    child_out.reset();
    child_err.reset();
    if (!ok) {
        result.start_error = "CreateProcess failed: " + std::to_string(GetLastError());
        return result;
    }
    Handle process(pi.hProcess), thread(pi.hThread);
    if (job.h) AssignProcessToJobObject(job.h, process.h);
    ResumeThread(thread.h);
    result.started = true;

    lorica::input_budget out_budget({options.max_stdout, options.max_stdout, options.max_stdout, 1});
    lorica::input_budget err_budget({options.max_stderr, options.max_stderr, options.max_stderr, 1});
    std::mutex m;
    bool exceeded = false;
    auto kill_all = [&] {
        if (job.h) TerminateJobObject(job.h, 1);
        else TerminateProcess(process.h, 1);
    };
    auto reader = [&](HANDLE h, std::string& sink, lorica::input_budget& budget) {
        char buf[16384];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) {
            std::lock_guard lock(m);
            if (!budget.consume(n, 0, 0)) {
                exceeded = true;
                kill_all();
                return;
            }
            sink.append(buf, n);
        }
    };
    std::thread t_out(reader, parent_out.h, std::ref(result.out), std::ref(out_budget));
    std::thread t_err(reader, parent_err.h, std::ref(result.err), std::ref(err_budget));
    std::thread t_in([&] {
        std::size_t offset = 0;
        while (offset < options.stdin_data.size()) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(options.stdin_data.size() - offset, 65536));
            if (!WriteFile(parent_in.h, options.stdin_data.data() + offset, chunk, &written, nullptr)) break;
            offset += written;
        }
        parent_in.reset();
    });
    const auto deadline = started_at + options.timeout.count();
    bool killed = false;
    while (true) {
        const DWORD w = WaitForSingleObject(process.h, 100);
        if (w == WAIT_OBJECT_0) break;
        if (!killed && time::steady_ms() >= deadline) {
            result.timed_out = true;
            kill_all();
            killed = true;
        } else if (!killed && options.should_cancel && options.should_cancel()) {
            result.cancelled = true;
            kill_all();
            killed = true;
        }
    }
    t_in.join();
    t_out.join();
    t_err.join();
    result.output_exceeded = exceeded;
    DWORD code = 0;
    if (!killed && !exceeded && GetExitCodeProcess(process.h, &code)) result.exit_code = static_cast<int>(code);
    result.duration_ms = time::steady_ms() - started_at;
    return result;
}

#endif

} // namespace di::proc
