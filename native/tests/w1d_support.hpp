#pragma once
// w1-D のテスト共通の補助 (fixture の読み込み・順序を無視した JSON 比較・偽 HTTP サーバ)。
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "check.hpp"
#include "infra/http_client.hpp"
#include "infra/http_server.hpp"

namespace w1d {

inline di::Json load_fixture(const std::string& rel) {
    auto text = di::fs::read_file(di::test::fixtures_dir() + "/" + rel, 16u << 20);
    if (!text) throw std::runtime_error("fixture missing: " + rel);
    di::JsonLimits limits;
    limits.max_bytes = 16u << 20;
    return di::parse_json_or_throw(*text, limits);
}

/** キー順を無視して比べる (数値は値で比べる)。 */
inline nlohmann::json unordered(const di::Json& j) { return nlohmann::json::parse(j.dump()); }

inline void check_json_eq(const di::Json& actual, const di::Json& expected, const char* what) {
    if (unordered(actual) != unordered(expected))
        throw di::test::Failure(std::string(what) + "\n  actual:   " + actual.dump() + "\n  expected: " + expected.dump());
}

/** テスト内の偽 HTTP サーバ (loopback・port 0)。 */
struct FakeServer {
    std::shared_ptr<di::http::Router> router = std::make_shared<di::http::Router>();
    std::unique_ptr<di::http::Server> server;
    std::string base;
    void start() {
        di::http::ServerOptions o;
        o.host = "127.0.0.1";
        o.port = 0;
        o.handler_threads = 8;
        server = std::make_unique<di::http::Server>(o, router);
        server->start();
        base = "http://127.0.0.1:" + std::to_string(server->bound_port());
    }
    ~FakeServer() {
        if (server) server->stop();
    }
};

/** 偽サーバへ出すために allow_private_network だけを true にする fetch。 */
inline di::http::ClientResponse loopback_fetch(const di::http::ClientRequest& r) {
    auto copy = r;
    copy.allow_private_network = true;
    return di::http::fetch(copy);
}

inline std::string temp_dir(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / ("di-w1d-" + name + "-" + std::to_string(di::env::pid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}

} // namespace w1d
