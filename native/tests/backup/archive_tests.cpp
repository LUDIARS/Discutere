// src/backup/archive.ts の移植 (サイドカー呼び出し) のテスト。node + tsx で native/sidecar/backup-archive.ts を実際に起動する。
#include <filesystem>
#include <iostream>

#include "../check.hpp"
#include "backup/archive.hpp"
#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/process.hpp"
#include "base/strings.hpp"

using namespace di;

namespace {
bool sidecar_available() {
    auto root = env::get_nonempty("DI_REPO_ROOT");
    if (!root || !proc::find_executable("node", env::snapshot())) return false;
    env::set_for_test("DISCUTERE_REPO_ROOT", *root);
    return fs::is_dir(fs::join(*root, "node_modules/tar"));
}
}  // namespace

TEST_CASE("create_archive: 存在するものだけ含め、無いものは skip する") {
    if (!sidecar_available()) {
        std::cout << "skip: node / tar が無い\n";
        return;
    }
    auto dir = std::filesystem::temp_directory_path() / ("di-archive-test-" + crypto::uuid_v4());
    std::filesystem::create_directories(dir / "kg");
    const std::string d = fs::from_path(std::filesystem::canonical(dir));
    CHECK(fs::write_file_atomic(fs::join(d, "a.db"), "sqlite-ish"));
    CHECK(fs::write_file_atomic(fs::join(d, "kg/b.db"), "kg"));
    backup::CreateArchiveArgs args;
    args.targets = {"a.db", fs::join(d, "kg"), "missing.db"};
    args.outFile = fs::join(d, "out.tar.gz");
    args.cwd = d;
    const auto r = backup::create_archive(args);
    CHECK_EQ(json_stringify(backup::to_json(r)),
             json_stringify(Json{{"outFile", args.outFile}, {"included", {"a.db", "kg"}}, {"skipped", {"missing.db"}}}));
    const auto out = fs::read_file(args.outFile);
    CHECK(out && out->size() > 2 && static_cast<unsigned char>((*out)[0]) == 0x1f && static_cast<unsigned char>((*out)[1]) == 0x8b);
    // 対象が 1 件も無ければ失敗 (TS と同じ文言)
    args.targets = {"nothing"};
    try {
        backup::create_archive(args);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(str::contains(e.what(), "アーカイブ対象が 1 件も存在しません"));
    }
    std::filesystem::remove_all(dir);
}

DI_TEST_MAIN()
