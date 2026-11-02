// src/config.ts の移植の互換テスト (golden: native/tests/golden/w1-A-config.gen.ts)
// + TS テスト (tests/flow/vote-lenses.test.ts / foundation.test.ts / core/query-embed-cache.test.ts) の config 部分。
#include <filesystem>
#include <iostream>
#include <map>

#include "../check.hpp"
#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "config.hpp"

using namespace di;

namespace {

std::string make_temp_dir() {
    auto dir = std::filesystem::temp_directory_path() / ("di-config-test-" + crypto::uuid_v4());
    std::filesystem::create_directories(dir);
    return fs::from_path(std::filesystem::canonical(dir));
}

std::map<std::string, std::string> env_of(const Json& j) {
    std::map<std::string, std::string> env;
    for (auto it = j.begin(); it != j.end(); ++it) env[it.key()] = it.value().get<std::string>();
    return env;
}

struct Run {
    bool ok = false;
    std::string error;
    Json config;
};

Run run_case(const std::map<std::string, std::string>& env, const std::optional<std::string>& file_text,
             const std::string& file_name = "discutere.golden.json") {
    const std::string dir = make_temp_dir();
    if (file_text) CHECK(fs::write_file_atomic(fs::join(dir, file_name), *file_text));
    Run r;
    try {
        config::LoadOptions o;
        o.env = env;
        o.cwd = dir;
        const auto c = config::load_config(o);
        r.ok = true;
        r.config = parse_json_or_throw(str::replace_all(json_stringify(config::to_json(c)), dir, "<CWD>"));
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    std::filesystem::remove_all(fs::to_path(dir));
    return r;
}

}  // namespace

TEST_CASE("golden: loadConfig と同じ JSON (合成 env + 合成 config ファイル)") {
    log::set_console_enabled(false);
    auto text = fs::read_file(test::fixtures_dir() + "/config/load.json");
    CHECK(text.has_value());
    const Json cases = parse_json_or_throw(*text, JsonLimits{8u << 20, 1'000'000, 64});
    CHECK(cases.size() >= 10);
    for (const auto& c : cases) {
        const std::string name = c["name"].get<std::string>();
        std::optional<std::string> file;
        if (c["file"].is_string()) file = c["file"].get<std::string>();
        const Run r = run_case(env_of(c["env"]), file);
        CHECK_EQ(r.ok, c["ok"].get<bool>());
        if (r.ok) {
            if (json_stringify(r.config) != json_stringify(c["config"])) {
                std::cerr << "case " << name << "\n  actual:   " << json_stringify(r.config)
                          << "\n  expected: " << json_stringify(c["config"]) << "\n";
            }
            CHECK_EQ(json_stringify(r.config), json_stringify(c["config"]));
        } else {
            CHECK_EQ(r.error, c["error"].get<std::string>());
        }
    }
}

TEST_CASE("flow 既定と env 上書き (foundation.test / vote-lenses.test)") {
    const Run d = run_case({{"DISCUTERE_CONFIG", "./none.json"}}, std::nullopt);
    CHECK(d.ok);
    CHECK_EQ(d.config["flow"]["rounds"].get<double>(), 3.0);
    CHECK_EQ(d.config["flow"]["voteLenses"].size(), std::size_t{3});
    CHECK_EQ(d.config["flow"]["voteLenses"][0]["key"].get<std::string>(), std::string("logic"));
    CHECK_EQ(d.config["flow"]["convergeShare"].get<double>(), 0.6);
    CHECK_EQ(d.config["flow"]["personaSetupModel"].get<std::string>(), std::string(""));
    const Run e = run_case({{"DISCUTERE_FLOW_ROUNDS", "7"},
                            {"DISCUTERE_FLOW_TAGS", "機密,開発"},
                            {"DISCUTERE_FLOW_VOTE_LENSES", R"([{"key":"cost","instruction":"コスト"}])"}},
                           std::nullopt);
    CHECK_EQ(e.config["flow"]["rounds"].get<double>(), 7.0);
    CHECK_EQ(json_stringify(e.config["flow"]["tags"]), std::string(R"(["機密","開発"])"));
    CHECK_EQ(e.config["flow"]["voteLenses"].size(), std::size_t{1});
}

TEST_CASE("embedding.batchSize は正の整数のみ (query-embed-cache.test)") {
    for (const char* v : {"0", "-1", "1.5", "abc"}) {
        const Run r = run_case({{"DISCUTERE_EMBEDDING_BATCH_SIZE", v}}, std::nullopt);
        CHECK(!r.ok);
        CHECK(str::contains(r.error, "embedding.batchSize must be a positive integer"));
    }
}

TEST_CASE("境界: config ファイルの上限ちょうど・上限+1・壊れた入力") {
    log::set_console_enabled(false);
    // 上限ちょうど (空白で水増しした有効な JSON) は読む。
    std::string body = R"({"workspace":"ws-limit"})";
    std::string at_limit = body + std::string(config::kMaxConfigFileBytes - body.size(), ' ');
    CHECK_EQ(at_limit.size(), config::kMaxConfigFileBytes);
    const Run a = run_case({{"DISCUTERE_CONFIG", "./c.json"}}, at_limit, "c.json");
    CHECK_EQ(a.config["workspace"].get<std::string>(), std::string("ws-limit"));
    // 上限+1 は読まずに既定値 (ファイルは無視)。
    const Run b = run_case({{"DISCUTERE_CONFIG", "./c.json"}}, at_limit + " ", "c.json");
    CHECK(b.ok);
    CHECK_EQ(b.config["workspace"].get<std::string>(), std::string("knowledge"));
    // 深すぎる入れ子は壊れた入力として無視。
    const Run deep = run_case({{"DISCUTERE_CONFIG", "./c.json"}}, std::string(1000, '[') + std::string(1000, ']'), "c.json");
    CHECK(deep.ok);
    CHECK_EQ(deep.config["workspace"].get<std::string>(), std::string("knowledge"));
    // 巨大な env の投票レンズ JSON は既定レンズに戻る。
    std::string huge = "[" + std::string(200'000, ' ') + "]";
    const Run h = run_case({{"DISCUTERE_FLOW_VOTE_LENSES", huge}}, std::nullopt);
    CHECK_EQ(h.config["flow"]["voteLenses"].size(), std::size_t{3});
    // config ファイルがディレクトリなら無視。
    const Run dir = run_case({{"DISCUTERE_CONFIG", "."}}, std::nullopt);
    CHECK(dir.ok);
}

TEST_CASE("get_config はキャッシュし、reset で読み直す") {
    env::set_for_test("DISCUTERE_CONFIG", "./__no_such_config__.json");
    env::set_for_test("DISCATIER_WORKSPACE", "ws-a");
    config::reset_config_for_test();
    CHECK_EQ(config::get_config().workspace, std::string("ws-a"));
    env::set_for_test("DISCATIER_WORKSPACE", "ws-b");
    CHECK_EQ(config::get_config().workspace, std::string("ws-a"));
    config::reset_config_for_test();
    CHECK_EQ(config::get_config().workspace, std::string("ws-b"));
    env::set_for_test("DISCATIER_WORKSPACE", std::nullopt);
    env::set_for_test("DISCUTERE_CONFIG", std::nullopt);
    config::reset_config_for_test();
}

DI_TEST_MAIN()
