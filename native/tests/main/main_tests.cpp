// 起動の結合テスト (w6-main): discutere-server を子プロセスで起動し、組み立て (mount 順・CORS・認可・終了) を外から確かめる。
//
// - 一時ディレクトリを cwd にし、合成の env だけを渡す (親の env は PATH 以外渡さない)。Discord token 無し・
//   LLM は claude-cli (起動時には呼ばない。PATH に claude が無くてもよい)。外部サービスには繋がない。
// - port は空きを探して渡す。終了は SIGTERM (Vestigium の JSONL に出る pid を使う)。テストの終わりに必ず子を止める。
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "base/crypto.hpp"
#include "base/env.hpp"
#include "base/fsutil.hpp"
#include "base/json.hpp"
#include "base/process.hpp"
#include "base/strings.hpp"
#include "check.hpp"
#include "infra/http.hpp"
#include "infra/http_client.hpp"
#include "infra/http_server.hpp"

#ifndef _WIN32
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

using namespace di;


namespace {

#ifndef _WIN32

/** テスト実行ファイル (build/tests/…) から build/bin/discutere-server を探す。DI_SERVER_BIN で上書き可。 */
std::string server_binary() {
    if (auto e = env::get_nonempty("DI_SERVER_BIN")) return *e;
    std::error_code ec;
    auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    for (auto dir = self.parent_path(); !dir.empty() && dir != dir.root_path(); dir = dir.parent_path()) {
        auto candidate = dir / "bin" / "discutere-server";
        if (std::filesystem::exists(candidate)) return candidate.string();
    }
    return {};
}

/** port=0 で一度 bind して空きポートを得る。 */
std::uint16_t free_port() {
    auto router = std::make_shared<http::Router>();
    http::ServerOptions o;
    o.host = "127.0.0.1";
    o.port = 0;
    http::Server s(o, router);
    s.start();
    auto p = s.bound_port();
    s.stop();
    return p;
}

http::ClientResponse request(std::uint16_t port, const std::string& method, const std::string& path,
                             std::map<std::string, std::string> headers = {}) {
    http::ClientRequest r;
    r.method = method;
    r.url = "http://127.0.0.1:" + std::to_string(port) + path;
    for (auto& [k, v] : headers) r.headers.set(k, v);
    r.allow_private_network = true;  // loopback の自分のサーバ
    r.timeout = std::chrono::milliseconds(15'000);
    r.connect_timeout = std::chrono::milliseconds(2'000);
    r.max_redirects = 0;
    return http::fetch(r);
}

/** 子プロセスを背景のスレッドで走らせる。破棄時に必ず止めて待つ。 */
class ServerProcess {
public:
    ServerProcess(std::string dir, std::map<std::string, std::string> env) {
        proc::Options o;
        o.argv = {server_binary()};
        o.env = std::move(env);
        o.cwd = std::move(dir);
        o.timeout = std::chrono::milliseconds(110'000);
        o.max_stdout = 8u << 20;
        o.max_stderr = 8u << 20;
        auto cancel = cancel_;
        o.should_cancel = [cancel] { return cancel->load(); };
        thread_ = std::thread([this, o] {
            result_ = proc::run(o);
            done_->store(true);
        });
    }
    ~ServerProcess() {
        cancel_->store(true);
        if (thread_.joinable()) thread_.join();
    }
    bool done() const { return done_->load(); }
    /** 終わるまで待つ (期限内に終われば true)。 */
    bool wait(std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!done()) return false;
        if (thread_.joinable()) thread_.join();
        return true;
    }
    const proc::Result& result() const { return result_; }

private:
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> done_ = std::make_shared<std::atomic<bool>>(false);
    std::thread thread_;
    proc::Result result_;
};

/** Vestigium の JSONL から子の pid を読む。 */
std::optional<long long> pid_from_logs(const std::string& logs_dir) {
    for (const auto& name : fs::list_dir(fs::join(logs_dir, "discutere"))) {
        auto text = fs::read_file(fs::join(fs::join(logs_dir, "discutere"), name), 4u << 20);
        if (!text) continue;
        for (const auto& line : str::split_lines(*text)) {
            auto j = parse_json(line, JsonLimits{});
            if (j && j->is_object() && j->contains("pid") && (*j)["pid"].is_number_integer()) return (*j)["pid"].get<long long>();
        }
    }
    return std::nullopt;
}

struct TempDir {
    std::string path;
    TempDir() {
        auto p = std::filesystem::temp_directory_path() / ("di-w6-main-" + crypto::uuid_v4());
        std::filesystem::create_directories(p);
        path = p.string();
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::map<std::string, std::string> synthetic_env(const std::string& dir, std::uint16_t port) {
    std::map<std::string, std::string> e;
    e["PATH"] = env::get_or("PATH", "/usr/bin:/bin");
    e["HOME"] = dir;
    e["NODE_ENV"] = "test";
    e["BACKEND_HOST"] = "127.0.0.1";
    e["BACKEND_PORT"] = std::to_string(port);
    e["FRONTEND_URL"] = "http://localhost:5174";
    e["DISCUTERE_CONFIG"] = fs::join(dir, "no-such-config.json");
    e["VESTIGIUM_LOGS_DIR"] = fs::join(dir, "logs");
    e["LLM_BACKEND"] = "claude-cli";
    e["DISCUTERE_CLASSIFIER_BACKEND"] = "off";
    return e;
}

#endif

}  // namespace

#ifndef _WIN32

TEST_CASE("server_boots_serves_routes_and_stops_on_sigterm") {
    const std::string bin = server_binary();
    CHECK(!bin.empty());
    TempDir tmp;
    const auto port = free_port();
    const auto logs = fs::join(tmp.path, "logs");
    ServerProcess server(tmp.path, synthetic_env(tmp.path, port));

    // /health が返るまで待つ (起動の失敗は子の出力を出して落とす)。
    bool up = false;
    for (int i = 0; i < 300 && !server.done(); ++i) {
        auto r = request(port, "GET", "/health");
        if (r.transport_ok && r.status == 200) {
            up = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!up) {
        server.wait(std::chrono::milliseconds(1));
        std::cout << server.result().out << "\n" << server.result().err << "\n";
    }
    CHECK(up);

    auto health = request(port, "GET", "/health");
    CHECK_EQ(health.body, std::string(R"({"status":"ok","service":"discutere"})"));
    CHECK(str::starts_with(health.headers.get("Content-Type").value_or(""), "application/json"));

    // 画面 (トップ / フロー / チャット) は HTML。/flow は LLM backend (claude-cli) があるので mount される。
    for (const char* path : {"/", "/flow", "/chat"}) {
        auto r = request(port, "GET", path);
        CHECK_EQ(r.status, 200);
        CHECK(str::starts_with(r.headers.get("Content-Type").value_or(""), "text/html"));
        CHECK(str::contains(r.body, "<html") || str::contains(r.body, "<!doctype") || str::contains(r.body, "<!DOCTYPE"));
    }

    // /api/* の userContext: loopback はヘッダ無しで admin。persona engine は注入済み (claude-cli backend)。
    auto status = request(port, "GET", "/api/admin/status");
    CHECK_EQ(status.status, 200);
    auto sj = parse_json(status.body, JsonLimits{});
    CHECK(sj && sj->is_object());
    // ヘッダで一般利用者を名乗ると admin ではない (403)。
    auto forbidden = request(port, "GET", "/api/admin/status", {{"X-User-Id", "u-1"}});
    CHECK_EQ(forbidden.status, 403);

    // 存在しない経路は Hono と同じ 404 "404 Not Found"。
    auto missing = request(port, "GET", "/no/such/route");
    CHECK_EQ(missing.status, 404);
    CHECK_EQ(missing.body, std::string("404 Not Found"));

    // CORS preflight: frontendUrl だけを許す。
    auto pre = request(port, "OPTIONS", "/api/admin/status",
                       {{"Origin", "http://localhost:5174"}, {"Access-Control-Request-Method", "POST"},
                        {"Access-Control-Request-Headers", "Content-Type"}});
    CHECK_EQ(pre.status, 204);
    CHECK_EQ(pre.headers.get("Access-Control-Allow-Origin").value_or(""), std::string("http://localhost:5174"));
    CHECK_EQ(pre.headers.get("Access-Control-Allow-Credentials").value_or(""), std::string("true"));
    CHECK(str::contains(pre.headers.get("Access-Control-Allow-Methods").value_or(""), "DELETE"));
    CHECK(str::contains(pre.headers.get("Access-Control-Allow-Headers").value_or(""), "X-User-Role"));

    // 内部の経路: worker callback は loopback から到達でき、worker-pool でないので 503。
    auto worker = request(port, "POST", "/internal/worker/register");
    CHECK_EQ(worker.status, 503);
    CHECK_EQ(worker.body, std::string(R"({"error":"worker pool not initialized"})"));
    // 外部会話への参加は共有秘密が無ければ 503 (default-deny)。
    auto ext = request(port, "POST", "/internal/external-discussion/consider");
    CHECK_EQ(ext.status, 503);

    // SIGTERM で graceful shutdown (終了コード 0・終了ログ)。
    auto pid = pid_from_logs(logs);
    CHECK(pid.has_value());
    CHECK_EQ(::kill(static_cast<pid_t>(*pid), SIGTERM), 0);
    CHECK(server.wait(std::chrono::milliseconds(30'000)));
    const auto& res = server.result();
    CHECK_EQ(res.exit_code, 0);
    CHECK(str::contains(res.out, "SIGTERM received"));
    CHECK(str::contains(res.out, "[discutere] HTTP listening on 127.0.0.1:" + std::to_string(port)));
    // 起動ログ: Discord token 無しは skip、persona engine は既定で無効。
    CHECK(str::contains(res.out, "discord-gateway: skipped"));
    CHECK(str::contains(res.out, "persona-engine: disabled"));
    CHECK(str::contains(res.out, "flow-ui: /flow (loopback)"));
    // 終わった後は繋がらない。
    auto after = request(port, "GET", "/health");
    CHECK(!after.transport_ok);
}

TEST_CASE("startup_guard_fails_fast_in_production_without_bot_token") {
    const std::string bin = server_binary();
    CHECK(!bin.empty());
    TempDir tmp;
    auto env = synthetic_env(tmp.path, free_port());
    env["NODE_ENV"] = "production";
    ServerProcess server(tmp.path, env);
    CHECK(server.wait(std::chrono::milliseconds(30'000)));
    CHECK(server.result().exit_code != 0);
    CHECK(str::contains(server.result().out + server.result().err, "startup guard"));
}

TEST_CASE("external_discussion_secret_without_model_fails_fast") {
    TempDir tmp;
    auto env = synthetic_env(tmp.path, free_port());
    env["DISCUTERE_EXTERNAL_DISCUSSION_SECRET"] = "fake-shared-secret-for-test-0000000000";
    // roster.discussants も llm.model (ANTHROPIC_MODEL) も既定は無い → TS と同じく起動を止める。
    ServerProcess server(tmp.path, env);
    CHECK(server.wait(std::chrono::milliseconds(30'000)));
    const auto& res = server.result();
    CHECK(res.exit_code != 0);
    CHECK(str::contains(res.out + res.err, "External discussion requires a configured model"));
}

#else

TEST_CASE("skipped_on_windows") { std::cout << "main の結合テストは POSIX のみ (SIGTERM を使う)\n"; }

#endif

DI_TEST_MAIN()
