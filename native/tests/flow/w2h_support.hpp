#pragma once
// w2-H のテストが共有する補助 (fixture flow/w2h.json・一時 DB)。
#include <filesystem>
#include <string>

#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "check.hpp"
#include "flow/db/connection.hpp"

namespace w2h {

inline const di::Json& fx() {
    static const di::Json doc = [] {
        auto text = di::fs::read_file(di::test::fixtures_dir() + "/flow/w2h.json", 32u << 20);
        if (!text) throw di::test::Failure("fixture missing: flow/w2h.json");
        return di::parse_json_or_throw(*text, {32u << 20, 5'000'000, 64});
    }();
    return doc;
}

inline std::string s(const di::Json& j) { return di::json_stringify(j); }

/** DATABASE_PATH を一時ディレクトリに向け、フロー DB を開き直す。 */
struct TempFlowDb {
    std::filesystem::path dir;
    TempFlowDb() {
        dir = std::filesystem::temp_directory_path() / ("di-w2h-" + di::crypto::uuid_v4());
        std::filesystem::create_directories(dir);
        di::env::set_for_test("DATABASE_PATH", di::fs::from_path(dir / "flow.db"));
        di::flow::db::reset_flow_db_for_test();
    }
    ~TempFlowDb() {
        di::flow::db::reset_flow_db_for_test();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string file(const std::string& name) const { return di::fs::from_path(dir / name); }
};

} // namespace w2h
