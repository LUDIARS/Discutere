#pragma once
// w1-C のデータ層テストの共通補助 (一時ディレクトリ・fixture・sqlite_master)。
#include <filesystem>
#include <string>

#include "base/crypto.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "check.hpp"

namespace di::test {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() / ("di-w1c-" + crypto::uuid_v4());
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string file(const std::string& name) const { return fs::from_path(path / name); }
};

inline Json load_fixture(const std::string& rel) {
    auto text = fs::read_file(fixtures_dir() + "/" + rel, 32u << 20);
    if (!text) throw Failure("fixture not found: " + rel);
    return parse_json_or_throw(*text, {32u << 20, 5'000'000, 64});
}

inline Json sqlite_master(sql::Database& db) {
    Json out = Json::array();
    for (const auto& row : db.all("SELECT type, name, tbl_name, sql FROM sqlite_master ORDER BY type, name"))
        out.push_back(row.to_json());
    return out;
}

inline Json rows_json(sql::Database& db, const std::string& sql) {
    Json out = Json::array();
    for (const auto& row : db.all(sql)) out.push_back(row.to_json());
    return out;
}

} // namespace di::test
