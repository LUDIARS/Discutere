#pragma once
// w3-N のテストの共通補助: 一時ディレクトリ・設定の env・golden の表を core に入れる。
#include <filesystem>
#include <string>

#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "base/sqlite.hpp"
#include "check.hpp"
#include "config.hpp"
#include "core/index.hpp"
#include "core/noise/exclusions.hpp"
#include "core/vectors/query_embed_cache.hpp"
#include "discord_hook/reactions.hpp"

namespace di::w3n {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() / ("di-w3n-" + crypto::uuid_v4());
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string file(const std::string& name) const { return fs::from_path(path / name); }
};

/** 実在の設定ファイルを読まず、KG を一時ディレクトリに向ける (埋め込みは有効、接続先は閉じたポート)。 */
struct TestConfig {
    explicit TestConfig(const TempDir& dir, bool embedding = true) {
        env::set_for_test("DISCUTERE_CONFIG", std::string_view(dir.file("no-config.json")));
        env::set_for_test("DISCATIER_KUZU_PATH", std::string_view(dir.file("kg/discatier.kuzu")));
        env::set_for_test("DISCUTERE_EMBEDDING_ENABLED", embedding ? std::optional<std::string_view>("1") : std::nullopt);
        env::set_for_test("DISCUTERE_EMBEDDING_MODEL", std::string_view("golden-model"));
        env::set_for_test("DISCUTERE_EMBEDDING_BASE_URL", std::string_view("http://127.0.0.1:9/v1"));
        config::reset_config_for_test();
        core::vectors::reset_query_embed_cache_for_test();
    }
    ~TestConfig() {
        for (const char* k : {"DISCUTERE_CONFIG", "DISCATIER_KUZU_PATH", "DISCUTERE_EMBEDDING_ENABLED", "DISCUTERE_EMBEDDING_MODEL",
                              "DISCUTERE_EMBEDDING_BASE_URL"})
            env::set_for_test(k, std::nullopt);
        config::reset_config_for_test();
        core::vectors::reset_query_embed_cache_for_test();
    }
};

inline Json load_fixture(const std::string& rel) {
    auto text = fs::read_file(test::fixtures_dir() + "/" + rel, 32u << 20);
    if (!text) throw test::Failure("fixture not found: " + rel);
    return parse_json_or_throw(*text, {32u << 20, 5'000'000, 64});
}

inline sql::Value to_value(const Json& v) {
    if (v.is_null()) return {};
    if (v.is_boolean()) return sql::Value(v.get<bool>());
    if (v.is_number_integer()) return sql::Value(v.get<long long>());
    if (v.is_number()) return sql::Value(v.get<double>());
    if (v.is_string()) return sql::Value(v.get<std::string>());
    return sql::Value(json_stringify(v));
}

/** golden の表 ({table: [row...]}, rowid 順) を同じ順で入れる (既存行は置き換える)。 */
inline void load_tables(core::Core& core, const Json& tables) {
    auto& db = core.raw();
    discord_hook::ensure_reaction_tables(db);
    core::noise::ensure_exclusion_table(db);
    core::vectors::ensure_query_cache_table(db);
    for (const auto& [table, rows] : tables.items()) {
        db.run("DELETE FROM " + table);
        for (const auto& row : rows) {
            std::string cols, marks;
            sql::Params params;
            for (const auto& [k, v] : row.items()) {
                if (!cols.empty()) {
                    cols += ", ";
                    marks += ", ";
                }
                cols += "\"" + k + "\"";
                marks += "?";
                params.add(to_value(v));
            }
            db.run("INSERT INTO " + table + " (" + cols + ") VALUES (" + marks + ")", params);
        }
    }
}

} // namespace di::w3n
