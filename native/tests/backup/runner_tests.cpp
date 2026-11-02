// w2-J: src/backup/runner.ts の移植。アーカイブ・S3 送信は注入した偽物で確かめる (本物の S3 には繋がない)。
#include <atomic>
#include <chrono>
#include <thread>

#include "check.hpp"
#include "core/test_util.hpp"

#include "backup/runner.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/strings.hpp"

using namespace di;
using namespace di::backup;

namespace {
config::DiscutereConfig base_config(const test::TempDir& dir) {
    config::DiscutereConfig c;
    c.workspace = "knowledge";
    c.backup.enabled = true;
    c.backup.bucket = "bkt";
    c.backup.prefix = "discutere";
    c.backup.region = "ap-northeast-1";
    c.backup.storageClass = "GLACIER";
    c.backup.intervalDays = 30;
    c.backup.stateFile = dir.file("state/backup-state.json");
    c.personaEngine.dbPath = "./data/persona-engine.db";
    c.fundamentum.enabled = true;
    c.fundamentum.dataDir = "./data/fundamentum";
    return c;
}
} // namespace

TEST_CASE("run_backup: bucket 未設定は ok=false (throw しない)") {
    test::TempDir dir;
    auto c = base_config(dir);
    c.backup.bucket = std::nullopt;
    const auto r = run_backup(c);
    CHECK(!r.ok && r.error == std::string("backup.bucket が未設定です (S3 バックアップ不可)"));
    CHECK_EQ(json_stringify(to_json(r)), json_stringify(Json{{"ok", false}, {"error", *r.error}}));
}

TEST_CASE("run_backup: 対象・key・state・一時ファイルの後始末") {
    test::TempDir dir;
    auto c = base_config(dir);
    c.discatier.kuzuPath = "./data/kg/x.kuzu";
    env::set_for_test("DATABASE_PATH", std::nullopt);
    std::vector<std::string> targets;
    std::string archived;
    S3UploadArgs sent;
    BackupDeps deps;
    deps.tmpDir = dir.file("tmp");
    deps.now = [] { return std::int64_t{1'800'000'000'000}; };
    deps.createArchive = [&](const CreateArchiveArgs& a) {
        targets = a.targets;
        archived = a.outFile;
        CHECK(fs::write_file_atomic(a.outFile, "gz"));
        return CreateArchiveResult{a.outFile, {"data/kg/x.kuzu"}, {"data/discutere.db"}};
    };
    deps.uploadFileToS3 = [&](const S3UploadArgs& u) {
        sent = u;
        return S3UploadResult{u.bucket, u.key, 2, "GLACIER"};
    };
    const auto r = run_backup(c, deps);
    CHECK(r.ok);
    CHECK(targets == (std::vector<std::string>{"./data/kg/x.kuzu", "./data/persona-engine.db", "./data/discutere.db", "./data/fundamentum"}));
    CHECK(str::starts_with(sent.key, "discutere/knowledge/discutere-backup-") && str::ends_with(sent.key, ".tar.gz"));
    CHECK_EQ(sent.key.size(), std::string("discutere/knowledge/discutere-backup-YYYYMMDD-HHmmss.tar.gz").size());
    CHECK(sent.storageClass == std::string("GLACIER") && sent.filePath == archived);
    CHECK(!fs::exists(archived));  // 一時ファイルは消す
    const auto state = read_backup_state(c.backup.stateFile);
    CHECK(state.lastRunAt == 1'800'000'000'000.0 && state.lastKey == sent.key);
    CHECK_EQ(json_stringify(to_json(r)),
             json_stringify(Json{{"ok", true}, {"key", sent.key}, {"bucket", "bkt"}, {"bytes", 2}, {"storageClass", "GLACIER"},
                                 {"included", {"data/kg/x.kuzu"}}, {"skipped", {"data/discutere.db"}}}));
    // 失敗は ok=false + 例外の文言
    deps.uploadFileToS3 = [](const S3UploadArgs&) -> S3UploadResult { throw std::runtime_error("S3 upload failed: 403"); };
    const auto f = run_backup(c, deps);
    CHECK(!f.ok && f.error == std::string("S3 upload failed: 403"));
    // prefix 末尾の / は重ねない
    c.backup.prefix = "p/";
    CHECK(str::starts_with(build_backup_key(c, 0), "p/knowledge/discutere-backup-"));
}

TEST_CASE("backup state: 壊れた・大きすぎる・型違いのファイルは空の状態") {
    test::TempDir dir;
    CHECK(!read_backup_state(dir.file("none.json")).lastRunAt);
    CHECK(fs::write_file_atomic(dir.file("bad.json"), "{broken"));
    CHECK(!read_backup_state(dir.file("bad.json")).lastRunAt);
    CHECK(fs::write_file_atomic(dir.file("big.json"), "{\"lastRunAt\":1," + std::string(kMaxBackupStateBytes, ' ') + "}"));
    CHECK(!read_backup_state(dir.file("big.json")).lastRunAt);
    CHECK(fs::write_file_atomic(dir.file("type.json"), "{\"lastRunAt\":\"x\",\"lastKey\":3}"));
    const auto s = read_backup_state(dir.file("type.json"));
    CHECK(!s.lastRunAt && !s.lastKey);
}

TEST_CASE("scheduler: 無効なら trigger だけ生きる / overdue なら 1 回だけ走る / stop で止まる") {
    test::TempDir dir;
    auto c = base_config(dir);
    std::atomic<int> uploads{0};
    BackupSchedulerOptions o;
    o.deps.tmpDir = dir.file("tmp");
    o.deps.createArchive = [](const CreateArchiveArgs& a) { return CreateArchiveResult{a.outFile, {}, {}}; };
    o.deps.uploadFileToS3 = [&](const S3UploadArgs& u) {
        ++uploads;
        return S3UploadResult{u.bucket, u.key, 0, "GLACIER"};
    };
    o.kickoffDelayMs = 10;
    o.tickMs = 20;
    auto disabled = c;
    disabled.backup.enabled = false;
    disabled.backup.stateFile = dir.file("state/disabled.json");
    auto h0 = start_backup_scheduler(disabled, o);
    CHECK(h0->trigger().ok);
    CHECK_EQ(uploads.load(), 1);
    h0->stop();

    auto h = start_backup_scheduler(c, o);
    for (int i = 0; i < 200 && uploads.load() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // 1 回走ると state が新しくなり、以後の tick は interval (30 日) 未満なので走らない
    CHECK_EQ(uploads.load(), 2);
    h->stop();
    h->stop();
}

DI_TEST_MAIN()
