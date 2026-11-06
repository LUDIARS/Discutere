#include "flow/contracts/runtime.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"

namespace di::flow::contracts {

namespace {

constexpr const char* ACTIVE_FILE = "contracts.jsonl";
constexpr const char* ARCHIVE_DIR = "archive";
constexpr int DEFAULT_RETENTION_DAYS = 7;
constexpr std::int64_t DAY_OFFSET_MS = 9LL * 60 * 60 * 1000;
constexpr std::int64_t DAY_MS = 24LL * 60 * 60 * 1000;

std::mutex& mutex() {
    static std::mutex m;
    return m;
}
// 有効ファイルごとに、このプロセスがその日のものだけと確かめた日
std::map<std::string, std::string>& confirmed_day() {
    static std::map<std::string, std::string> m;
    return m;
}

bool is_digits(std::string_view s) {
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return !s.empty();
}

// /^\.?contracts-(\d{4}-\d{2}-\d{2})(?:\..*)?\.(?:jsonl|part)$/ の日付部分
std::optional<std::string> archive_day_of(std::string_view name) {
    if (!name.empty() && name[0] == '.') name.remove_prefix(1);
    if (!str::starts_with(name, "contracts-") || name.size() < 10 + 10) return std::nullopt;
    const std::string_view day = name.substr(10, 10);
    if (!(is_digits(day.substr(0, 4)) && day[4] == '-' && is_digits(day.substr(5, 2)) && day[7] == '-' && is_digits(day.substr(8, 2))))
        return std::nullopt;
    std::string_view rest = name.substr(20);
    std::string_view ext;
    if (str::ends_with(rest, ".jsonl")) ext = ".jsonl";
    else if (str::ends_with(rest, ".part")) ext = ".part";
    else return std::nullopt;
    rest.remove_suffix(ext.size());
    // 残りは空か "." で始まる任意の文字列 ((?:\..*)?。. は改行に一致しない)
    if (!rest.empty() && (rest[0] != '.' || rest.find('\n') != std::string_view::npos || rest.find('\r') != std::string_view::npos))
        return std::nullopt;
    return std::string(day);
}

std::int64_t mtime_ms(const std::filesystem::path& p) {
    const auto ft = std::filesystem::last_write_time(p);
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
    return std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
}

// 有効ファイルを archive へ移す。rename が「取った」印 (勝つのは 1 プロセスだけ)。
// archive 名が空いていれば hard link で公開し、無ければ既存の archive へ追記する。
void archive_day(const std::string& directory, const std::string& active, const std::string& day, std::int64_t now) {
    const std::string archive = fs::join(directory, ARCHIVE_DIR);
    fs::ensure_dir(archive);
    const std::string claimed =
        fs::join(archive, ".contracts-" + day + "." + std::to_string(env::pid()) + "." + std::to_string(now) + ".part");
    std::filesystem::rename(fs::to_path(active), fs::to_path(claimed));  // ENOENT は呼び出し側で扱う
    const std::string target = fs::join(archive, "contracts-" + day + ".jsonl");
    std::error_code ec;
    std::filesystem::create_hard_link(fs::to_path(claimed), fs::to_path(target), ec);
    if (ec) {
        if (ec != std::errc::file_exists) throw std::filesystem::filesystem_error("link", ec);
        // archive は自分たちが書いたログ。大きさの上限は有効ファイル 1 日分として 512MiB で切る。
        if (auto data = fs::read_file(claimed, 512u << 20)) fs::append_file(target, *data);
    }
    std::filesystem::remove(fs::to_path(claimed), ec);
}

void prune_archive(const std::string& directory, const std::string& today) {
    // 日付ラベルは日付として比べるので、算術もラベル自体で行う。
    const auto parsed = time::parse_iso(today + "T00:00:00Z");
    if (!parsed) return;
    const std::string cutoff = time::iso_utc(*parsed - static_cast<std::int64_t>(retention_days()) * DAY_MS).substr(0, 10);
    const std::string dir = fs::join(directory, ARCHIVE_DIR);
    for (const auto& name : fs::list_dir(dir)) {
        const auto day = archive_day_of(name);
        if (day && *day < cutoff) {
            std::error_code ec;
            std::filesystem::remove(fs::to_path(fs::join(dir, name)), ec);
        }
    }
}

void rotate_if_stale(const std::string& directory, const std::string& active, const std::string& today, std::int64_t now) {
    auto& confirmed = confirmed_day();
    auto it = confirmed.find(active);
    if (it != confirmed.end() && it->second == today) return;
    std::int64_t modified = 0;
    try {
        if (!std::filesystem::exists(fs::to_path(active))) {
            // まだ有効ファイルが無い: 下の追記が今日のファイルを作る。
            confirmed[active] = today;
            return;
        }
        modified = mtime_ms(fs::to_path(active));
    } catch (...) {
        confirmed[active] = today;
        return;
    }
    const std::string day = day_key(modified);
    if (day < today) {
        try {
            archive_day(directory, active, day, now);
        } catch (const std::filesystem::filesystem_error& e) {
            // ENOENT: 別プロセスが先に切り替えた。それ以外 (Windows の共有違反など) は次の書き込みで再試行する。
            if (e.code() != std::errc::no_such_file_or_directory) return;
        }
        prune_archive(directory, today);
    }
    confirmed[active] = today;
}

} // namespace

std::string day_key(std::int64_t epoch_ms) { return time::iso_utc(epoch_ms + DAY_OFFSET_MS).substr(0, 10); }

int retention_days() {
    const auto raw = env::get("AUGUR_CONTRACT_LOG_RETENTION_DAYS");
    if (!raw || str::trim(*raw).empty()) return DEFAULT_RETENTION_DAYS;
    const auto n = str::js_number(*raw);
    if (!n || *n != static_cast<double>(static_cast<long long>(*n)) || *n < 0 || *n > 1e6) return DEFAULT_RETENTION_DAYS;
    return static_cast<int>(*n);
}

void record(const std::string& message, const Context& context, std::optional<std::int64_t> now_ms) {
    try {
        std::lock_guard lock(mutex());
        const std::string directory = env::get_nonempty("VESTIGIUM_LOGS_DIR").value_or(fs::join(env::cwd(), "logs"));
        fs::ensure_dir(directory);
        const std::string active = fs::join(directory, ACTIVE_FILE);
        const std::int64_t now = now_ms.value_or(time::now_ms());
        try {
            rotate_if_stale(directory, active, day_key(now), now);
        } catch (...) {
            // 切替は片付け仕事。記録自体は書く。
        }
        Json ctx = Json::object();
        for (const auto& [k, v] : context) ctx[k] = v;
        Json line = Json::object();
        line["time"] = time::iso_utc(now);
        line["msg"] = message;
        line["ctx"] = std::move(ctx);
        fs::append_file(active, json_stringify(line) + "\n");
    } catch (...) {
        // 観測は包んだ処理を決して変えない。
    }
}

void reset_for_test() {
    std::lock_guard lock(mutex());
    confirmed_day().clear();
}

namespace detail {

Context base_context(const ContractSpec& spec) {
    return {{"contract", spec.contractId}, {"id", spec.id}, {"where", spec.where}, {"observed_at", time::now_iso()}};
}

Context with(Context ctx, const char* phase, const std::string& reason) {
    ctx.emplace_back("phase", phase);
    ctx.emplace_back("reason", reason);
    return ctx;
}

} // namespace detail

} // namespace di::flow::contracts
