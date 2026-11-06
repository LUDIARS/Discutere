#include "backup/runner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <limits>
#include <mutex>

#include "base/env.hpp"
#include "base/executor.hpp"
#include "base/fsutil.hpp"
#include "base/log.hpp"
#include "base/time.hpp"
#include "core/kg_registry.hpp"

namespace di::backup {

namespace {

constexpr std::int64_t DAY_MS = 24LL * 60 * 60 * 1000;

std::int64_t now_of(const BackupDeps& deps) { return deps.now ? deps.now() : time::now_ms(); }

std::string pad2(int n) { return n < 10 ? "0" + std::to_string(n) : std::to_string(n); }

std::tm local_tm(std::int64_t ms) {
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

} // namespace

Json to_json(const BackupResult& r) {
    Json j = Json::object();
    j["ok"] = r.ok;
    if (r.key) j["key"] = *r.key;
    if (r.bucket) j["bucket"] = *r.bucket;
    if (r.bytes) j["bytes"] = *r.bytes;
    if (r.storageClass) j["storageClass"] = *r.storageClass;
    if (r.included) j["included"] = *r.included;
    if (r.skipped) j["skipped"] = *r.skipped;
    if (r.error) j["error"] = *r.error;
    return j;
}

BackupState read_backup_state(const std::string& state_file) {
    BackupState s;
    if (!fs::exists(state_file)) return s;
    auto text = fs::read_file(state_file, kMaxBackupStateBytes);
    if (!text) return s;
    auto j = parse_json(*text, {kMaxBackupStateBytes, 1000, 8});
    if (!j || !j->is_object()) return s;
    // TS は JSON.parse の結果をそのまま使う。数・文字列でない値は無いものとして扱う。
    s.lastRunAt = json_opt_number(*j, "lastRunAt");
    s.lastKey = json_opt_string(*j, "lastKey");
    return s;
}

void write_backup_state(const std::string& state_file, const BackupState& state) {
    fs::ensure_dir(fs::dirname(fs::resolve(state_file)));
    Json j = Json::object();
    if (state.lastRunAt) j["lastRunAt"] = *state.lastRunAt;
    if (state.lastKey) j["lastKey"] = *state.lastKey;
    if (!fs::write_file_atomic(state_file, json_stringify_pretty(j, 2))) throw std::runtime_error("failed to write backup state");
}

std::vector<std::string> resolve_backup_targets(const config::DiscutereConfig& config) {
    const std::string db_path = env::get("DATABASE_PATH").value_or("./data/discutere.db");
    std::vector<std::string> targets{
        // active KG SQLite index (§12)。未解決時は従来既定。
        core::resolve_active_kg_path(config).value_or("./data/discatier.kuzu"),
        config.personaEngine.dbPath,
        db_path,
    };
    // 議論データ管理ツール (Fundamentum export) の on-disk store
    if (config.fundamentum.enabled) targets.push_back(config.fundamentum.dataDir);
    return targets;
}

std::string build_backup_key(const config::DiscutereConfig& config, std::int64_t now_ms) {
    const std::tm d = local_tm(now_ms);
    const std::string ts = std::to_string(d.tm_year + 1900) + pad2(d.tm_mon + 1) + pad2(d.tm_mday) + "-" + pad2(d.tm_hour) +
                           pad2(d.tm_min) + pad2(d.tm_sec);
    const std::string& p = config.backup.prefix;
    const std::string prefix = !p.empty() && p.back() == '/' ? p : p + "/";
    return prefix + config.workspace + "/discutere-backup-" + ts + ".tar.gz";
}

BackupResult run_backup(const config::DiscutereConfig& config, const BackupDeps& deps) {
    BackupResult result;
    if (!config.backup.bucket || config.backup.bucket->empty()) {
        result.error = "backup.bucket が未設定です (S3 バックアップ不可)";
        return result;
    }
    const std::string tmp_dir = deps.tmpDir ? *deps.tmpDir : fs::join(fs::from_path(std::filesystem::temp_directory_path()), "discutere-backup");
    fs::ensure_dir(tmp_dir);
    const std::string tmp_file =
        fs::join(tmp_dir, "backup-" + std::to_string(env::pid()) + "-" + std::to_string(now_of(deps)) + ".tar.gz");
    try {
        CreateArchiveArgs a;
        a.targets = resolve_backup_targets(config);
        a.outFile = tmp_file;
        const auto archive = deps.createArchive ? deps.createArchive(a) : create_archive(a);
        const std::string key = build_backup_key(config, now_of(deps));
        S3UploadArgs u;
        u.bucket = *config.backup.bucket;
        u.key = key;
        u.filePath = tmp_file;
        u.region = config.backup.region;
        u.storageClass = config.backup.storageClass;
        u.endpoint = config.backup.endpoint;
        u.accessKeyId = config.backup.accessKeyId;
        u.secretAccessKey = config.backup.secretAccessKey;
        const auto up = deps.uploadFileToS3 ? deps.uploadFileToS3(u) : upload_file_to_s3(u);
        write_backup_state(config.backup.stateFile, BackupState{static_cast<double>(now_of(deps)), key});
        result.ok = true;
        result.key = up.key;
        result.bucket = up.bucket;
        result.bytes = up.bytes;
        result.storageClass = up.storageClass;
        result.included = archive.included;
        result.skipped = archive.skipped;
    } catch (const std::exception& e) {
        result.ok = false;
        result.error = e.what();
    }
    fs::remove_file(tmp_file);
    return result;
}

namespace {

class NoopScheduler : public BackupSchedulerHandle {
public:
    NoopScheduler(config::DiscutereConfig config, BackupDeps deps) : config_(std::move(config)), deps_(std::move(deps)) {}
    void stop() override {}
    BackupResult trigger() override { return run_backup(config_, deps_); }

private:
    config::DiscutereConfig config_;
    BackupDeps deps_;
};

struct Shared {
    config::DiscutereConfig config;
    BackupDeps deps;
    std::int64_t interval_ms = 0;
    std::atomic<bool> running{false};
    std::atomic<bool> stopped{false};
};

void maybe_run(const std::shared_ptr<Shared>& s) {
    if (s->stopped) return;
    const auto state = read_backup_state(s->config.backup.stateFile);
    const double elapsed = state.lastRunAt && *state.lastRunAt != 0 ? static_cast<double>(now_of(s->deps)) - *state.lastRunAt
                                                                    : std::numeric_limits<double>::infinity();
    if (elapsed < static_cast<double>(s->interval_ms)) return;
    bool expected = false;
    if (!s->running.compare_exchange_strong(expected, true)) return;  // 前回の実行が終わっていない
    log::info("  backup: running scheduled backup (elapsed=" + js_number_to_string(std::isinf(elapsed) ? elapsed : std::round(elapsed / DAY_MS)) + "d)");
    const auto r = run_backup(s->config, s->deps);
    if (r.ok)
        log::info("  backup: uploaded s3://" + r.bucket.value_or("") + "/" + r.key.value_or("") + " (" + std::to_string(r.bytes.value_or(0)) +
                  " bytes, " + r.storageClass.value_or("") + ")");
    else
        log::warn("  backup: scheduled backup failed: " + r.error.value_or(""));
    s->running = false;
}

class TimedScheduler : public BackupSchedulerHandle {
public:
    TimedScheduler(std::shared_ptr<Shared> shared, const BackupSchedulerOptions& options) : shared_(std::move(shared)) {
        auto s = shared_;
        auto kick = [s] {
            // スケジューラのスレッドでは重い仕事をしない。
            if (!background_executor().submit([s] { maybe_run(s); })) log::warn("  backup: scheduled backup skipped (queue full)");
        };
        kickoff_ = global_scheduler().after(std::chrono::milliseconds(options.kickoffDelayMs), kick);
        daily_ = global_scheduler().every(std::chrono::milliseconds(options.tickMs), kick);
    }
    ~TimedScheduler() override { stop(); }
    void stop() override {
        if (shared_->stopped.exchange(true)) return;
        global_scheduler().cancel(kickoff_);
        global_scheduler().cancel(daily_);
    }
    BackupResult trigger() override { return run_backup(shared_->config, shared_->deps); }

private:
    std::shared_ptr<Shared> shared_;
    Scheduler::Id kickoff_ = 0;
    Scheduler::Id daily_ = 0;
};

} // namespace

std::unique_ptr<BackupSchedulerHandle> start_backup_scheduler(const config::DiscutereConfig& config, BackupSchedulerOptions options) {
    const bool bucket_set = config.backup.bucket && !config.backup.bucket->empty();
    if (!config.backup.enabled || !bucket_set) {
        log::info(std::string("  backup: scheduler skipped (enabled=") + (config.backup.enabled ? "true" : "false") +
                  ", bucket=" + (bucket_set ? "set" : "unset") + ")");
        return std::make_unique<NoopScheduler>(config, options.deps);
    }
    auto shared = std::make_shared<Shared>();
    shared->config = config;
    shared->deps = options.deps;
    shared->interval_ms = static_cast<std::int64_t>(std::max(1.0, config.backup.intervalDays) * static_cast<double>(DAY_MS));
    auto handle = std::make_unique<TimedScheduler>(shared, options);
    log::info("  backup: scheduler attached (every " + js_number_to_string(config.backup.intervalDays) + "d → s3://" +
              *config.backup.bucket + "/" + config.backup.prefix + ")");
    return handle;
}

} // namespace di::backup
