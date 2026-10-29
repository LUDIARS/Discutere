#include "base/log.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "base/strings.hpp"
#include "base/time.hpp"

namespace di::log {
namespace {

struct Registry {
    std::mutex mutex;
    std::map<int, Sink> sinks;
    int next_id = 1;
    bool console = true;
};

Registry& registry() {
    static Registry r;
    return r;
}

thread_local bool in_sink = false;

constexpr std::size_t kMaxMsgBytes = 64 * 1024;

std::string truncate_msg(std::string_view s) {
    if (s.size() <= kMaxMsgBytes) return std::string(s);
    // vestigium: 先頭 (max-32) バイトを UTF-8 として読み、「…(truncated N)」を付ける。
    std::string cut = str::sanitize_utf8(s.substr(0, kMaxMsgBytes - 32));
    return cut + "\xE2\x80\xA6(truncated " + std::to_string(s.size() - (kMaxMsgBytes - 32)) + ")";
}

} // namespace

std::string_view level_name(Level level) {
    switch (level) {
    case Level::trace: return "trace";
    case Level::debug: return "debug";
    case Level::info: return "info";
    case Level::warn: return "warn";
    case Level::error: return "error";
    case Level::fatal: return "fatal";
    }
    return "info";
}

int add_sink(Sink sink) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    const int id = r.next_id++;
    r.sinks.emplace(id, std::move(sink));
    return id;
}

void remove_sink(int id) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    r.sinks.erase(id);
}

void set_console_enabled(bool enabled) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    r.console = enabled;
}

void write(Level level, std::string_view message) {
    auto& r = registry();
    Record rec{level, std::string(message), time::now_ms()};
    if (in_sink) {
        // sink の中から呼ばれた: 再入でロックを取り直さず、標準エラーにだけ出す。
        std::fwrite(message.data(), 1, message.size(), stderr);
        std::fputc('\n', stderr);
        return;
    }
    std::lock_guard lock(r.mutex);
    if (r.console) {
        FILE* out = (level == Level::warn || level == Level::error || level == Level::fatal) ? stderr : stdout;
        std::fwrite(message.data(), 1, message.size(), out);
        std::fputc('\n', out);
        std::fflush(out);
    }
    in_sink = true;
    for (auto& [id, sink] : r.sinks) {
        try {
            sink(rec);
        } catch (...) {
            // sink の失敗でログ呼び出し元を落とさない。
        }
    }
    in_sink = false;
}

std::string sanitize_service_code(std::string_view code) {
    std::string lower = str::to_lower_ascii(code);
    bool ok = !lower.empty() && lower.size() <= 64;
    for (std::size_t i = 0; ok && i < lower.size(); ++i) {
        const char c = lower[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (i == 0 ? !alnum : !(alnum || c == '_' || c == '-')) ok = false;
    }
    if (!ok) throw std::invalid_argument("invalid service code");
    return lower;
}

std::string serialize_record(std::int64_t ts, Level level, std::string_view service, std::string_view channel,
                             std::string_view msg, std::int64_t pid) {
    Json rec = Json::object();
    rec["ts"] = ts;
    rec["level"] = std::string(level_name(level));
    rec["service"] = std::string(service);
    rec["channel"] = std::string(channel);
    rec["msg"] = truncate_msg(msg);
    rec["pid"] = pid;
    return json_stringify(rec) + "\n";
}

struct Vestigium::Impl {
    std::string service;
    std::string logs_dir;
    int retention_days;
    std::int64_t sweep_interval_ms;
    std::mutex mutex;
    std::ofstream stream;
    std::string current_ymd;
    int sink_id = 0;
    std::atomic<bool> stopping{false};
    std::condition_variable cv;
    std::mutex cv_mutex;
    std::thread sweeper;

    std::string service_dir() const { return fs::join(logs_dir, service); }
    std::string day_file(std::int64_t ms) const { return fs::join(service_dir(), time::ymd_utc(ms) + ".jsonl"); }

    void write_line(const Record& rec) {
        const bool is_err = rec.level == Level::warn || rec.level == Level::error || rec.level == Level::fatal;
        const std::string line = serialize_record(rec.ts_ms, rec.level, service, is_err ? "stderr" : "app", rec.message, env::pid());
        std::lock_guard lock(mutex);
        if (stopping) return;
        const std::string ymd = time::ymd_utc(rec.ts_ms);
        if (ymd != current_ymd || !stream.is_open()) {
            if (stream.is_open()) stream.close();
            fs::ensure_dir(service_dir());
            stream.open(fs::to_path(day_file(rec.ts_ms)), std::ios::binary | std::ios::app);
            current_ymd = ymd;
        }
        if (stream) {
            stream.write(line.data(), static_cast<std::streamsize>(line.size()));
            stream.flush();
        }
    }
};

Vestigium::Vestigium(VestigiumOptions options) : impl_(new Impl) {
    impl_->service = sanitize_service_code(options.service_code);
    if (!options.logs_dir.empty() && !str::trim(options.logs_dir).empty()) impl_->logs_dir = fs::resolve(options.logs_dir);
    else if (auto e = env::get_nonempty("VESTIGIUM_LOGS_DIR")) impl_->logs_dir = fs::resolve(*e);
    else impl_->logs_dir = fs::resolve("logs");
    impl_->retention_days = options.retention_days;
    impl_->sweep_interval_ms = options.sweep_interval_ms;
    fs::ensure_dir(impl_->service_dir());
    Impl* impl = impl_;
    impl_->sink_id = add_sink([impl](const Record& rec) { impl->write_line(rec); });
    if (impl_->sweep_interval_ms > 0 && impl_->retention_days > 0) {
        sweep(time::now_ms());
        impl_->sweeper = std::thread([this] {
            std::unique_lock lock(impl_->cv_mutex);
            while (!impl_->stopping) {
                impl_->cv.wait_for(lock, std::chrono::milliseconds(impl_->sweep_interval_ms));
                if (impl_->stopping) break;
                sweep(time::now_ms());
            }
        });
    }
}

Vestigium::~Vestigium() {
    shutdown();
    delete impl_;
}

void Vestigium::shutdown() {
    if (impl_->stopping.exchange(true)) return;
    remove_sink(impl_->sink_id);
    {
        std::lock_guard lock(impl_->cv_mutex);
    }
    impl_->cv.notify_all();
    if (impl_->sweeper.joinable()) impl_->sweeper.join();
    std::lock_guard lock(impl_->mutex);
    if (impl_->stream.is_open()) impl_->stream.close();
}

int Vestigium::sweep(std::int64_t now_ms) {
    if (impl_->retention_days <= 0 || !fs::is_dir(impl_->logs_dir)) return 0;
    const std::string today = time::ymd_utc(now_ms);
    const auto c = time::to_civil_utc(now_ms);
    const std::int64_t cutoff = time::from_civil_utc(c.year, c.month, c.day) -
                                static_cast<std::int64_t>(impl_->retention_days) * 86'400'000;
    int removed = 0;
    for (const auto& dir_name : fs::list_dir(impl_->logs_dir)) {
        const std::string dir = fs::join(impl_->logs_dir, dir_name);
        if (!fs::is_dir(dir)) continue;
        for (const auto& name : fs::list_dir(dir)) {
            // ^(\d{4})-(\d{2})-(\d{2})\.jsonl(\.gz)?$
            if (name.size() < 16) continue;
            const bool jsonl = name.size() == 16 && str::ends_with(name, ".jsonl");
            const bool gz = name.size() == 19 && str::ends_with(name, ".jsonl.gz");
            if (!jsonl && !gz) continue;
            const auto parsed = time::parse_iso(std::string_view(name).substr(0, 10));
            if (!parsed) continue;
            const std::string full = fs::join(dir, name);
            if (!fs::is_file(full)) continue;
            if (str::starts_with(name, today)) continue;
            if (*parsed < cutoff) {
                fs::remove_file(full);
                ++removed;
            }
        }
    }
    return removed;
}

const std::string& Vestigium::logs_dir() const { return impl_->logs_dir; }
std::string Vestigium::current_file(std::int64_t now_ms) const { return impl_->day_file(now_ms); }

} // namespace di::log
