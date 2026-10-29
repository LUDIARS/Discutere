#include "base/env.hpp"

#include <cstdlib>
#include <filesystem>
#include <mutex>

#include "base/strings.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
extern char** environ;
#endif

namespace di::env {
namespace {
std::mutex& env_mutex() {
    static std::mutex m;
    return m;
}
#ifdef _WIN32
std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
#endif
} // namespace

std::optional<std::string> get(std::string_view name) {
    std::lock_guard lock(env_mutex());
#ifdef _WIN32
    const std::wstring wname = widen(name);
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring value(n, L'\0');
    const DWORD written = GetEnvironmentVariableW(wname.c_str(), value.data(), n);
    value.resize(written);
    return narrow(value);
#else
    const std::string key(name);
    const char* v = std::getenv(key.c_str());
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

std::optional<std::string> get_nonempty(std::string_view name) {
    auto v = get(name);
    if (!v || str::trim(*v).empty()) return std::nullopt;
    return v;
}

std::string get_or(std::string_view name, std::string_view fallback) {
    auto v = get(name);
    return v ? *v : std::string(fallback);
}

std::map<std::string, std::string> snapshot() {
    std::lock_guard lock(env_mutex());
    std::map<std::string, std::string> out;
#ifdef _WIN32
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) return out;
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
        std::wstring_view entry(p);
        const auto eq = entry.find(L'=', 1);  // "=C:=..." のような先頭 '=' の項目を飛ばす
        if (eq == std::wstring_view::npos) continue;
        out[narrow(entry.substr(0, eq))] = narrow(entry.substr(eq + 1));
    }
    FreeEnvironmentStringsW(block);
#else
    for (char** p = environ; p && *p; ++p) {
        std::string_view entry(*p);
        const auto eq = entry.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        out[std::string(entry.substr(0, eq))] = std::string(entry.substr(eq + 1));
    }
#endif
    return out;
}

void set_for_test(std::string_view name, std::optional<std::string_view> value) {
    std::lock_guard lock(env_mutex());
#ifdef _WIN32
    const std::wstring wname = widen(name);
    if (value) SetEnvironmentVariableW(wname.c_str(), widen(*value).c_str());
    else SetEnvironmentVariableW(wname.c_str(), nullptr);
#else
    const std::string key(name);
    if (value) setenv(key.c_str(), std::string(*value).c_str(), 1);
    else unsetenv(key.c_str());
#endif
}

std::string cwd() {
    const auto u8 = std::filesystem::current_path().u8string();
    return std::string(u8.begin(), u8.end());
}

std::int64_t pid() {
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

} // namespace di::env
