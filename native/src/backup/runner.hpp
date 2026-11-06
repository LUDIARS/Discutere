#pragma once
// 学習データバックアップの実行 + 月次スケジューラ (src/backup/runner.ts)。
//
// - run_backup(): tar.gz 化 (サイドカー) → S3 push (SigV4) → state 更新。手動 (slash / npm script) と
//   自動 (scheduler) の両方から呼ぶ単一エントリ。
// - start_backup_scheduler(): config.backup.enabled なら、起動 30 秒後に overdue 判定で 1 回 +
//   以後 24h tick (di::global_scheduler) で intervalDays (既定 30 = 月次) 経過ごとに run_backup。
//   実行そのものは background_executor で行い、同時に 2 本は走らせない。
// 「使うのは S3 のアーカイバ」要件: storageClass=GLACIER 系で安価に長期保管。
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "backup/archive.hpp"
#include "backup/s3.hpp"
#include "base/json.hpp"
#include "config.hpp"

namespace di::backup {

struct BackupResult {
    bool ok = false;
    std::optional<std::string> key;
    std::optional<std::string> bucket;
    std::optional<std::int64_t> bytes;
    std::optional<std::string> storageClass;
    std::optional<std::vector<std::string>> included;
    std::optional<std::vector<std::string>> skipped;
    std::optional<std::string> error;
};
Json to_json(const BackupResult& r);

struct BackupState {
    std::optional<double> lastRunAt;
    std::optional<std::string> lastKey;
};
/** 状態ファイルの上限 (大きすぎる・壊れたファイルは空の状態)。 */
inline constexpr std::size_t kMaxBackupStateBytes = 64 * 1024;
BackupState read_backup_state(const std::string& state_file);
void write_backup_state(const std::string& state_file, const BackupState& state);

/** 注入点 (テスト用)。既定は create_archive / upload_file_to_s3 / 現在時刻。 */
struct BackupDeps {
    std::function<CreateArchiveResult(const CreateArchiveArgs&)> createArchive;
    std::function<S3UploadResult(const S3UploadArgs&)> uploadFileToS3;
    std::function<std::int64_t()> now;
    /** 一時ファイルを置くディレクトリ (既定 <tmp>/discutere-backup)。 */
    std::optional<std::string> tmpDir;
};

/** バックアップ対象の実ファイル群を config から導出する。 */
std::vector<std::string> resolve_backup_targets(const config::DiscutereConfig& config);

/** タイムスタンプ付き S3 key (prefix/workspace/discutere-backup-YYYYMMDD-HHmmss.tar.gz、時刻はローカル時刻)。 */
std::string build_backup_key(const config::DiscutereConfig& config, std::int64_t now_ms);

/** バックアップを 1 回実行する。bucket 未設定なら ok=false を返す (throw しない)。 */
BackupResult run_backup(const config::DiscutereConfig& config, const BackupDeps& deps = {});

class BackupSchedulerHandle {
public:
    virtual ~BackupSchedulerHandle() = default;
    virtual void stop() = 0;
    /** 手動トリガ (slash / API から共有)。呼んだスレッドで実行する。 */
    virtual BackupResult trigger() = 0;
};

struct BackupSchedulerOptions {
    BackupDeps deps;
    /** 起動直後の overdue チェックまでの遅延 (既定 30 秒)。 */
    std::int64_t kickoffDelayMs = 30'000;
    /** overdue 判定の間隔 (既定 24h)。 */
    std::int64_t tickMs = 24LL * 60 * 60 * 1000;
};

/**
 * 月次 (intervalDays) 自動バックアップを開始する。
 * enabled=false / bucket 未設定なら trigger だけ生きた no-op スケジューラを返す。
 */
std::unique_ptr<BackupSchedulerHandle> start_backup_scheduler(const config::DiscutereConfig& config,
                                                              BackupSchedulerOptions options = {});

} // namespace di::backup
