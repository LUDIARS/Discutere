// socket_mode のテスト (2/2)。準備は socket_mode_support.hpp。
#include "socket_mode_support.hpp"

using namespace socket_mode_support;

TEST_CASE("socket mode gives up after consecutive failures and never logs the token") {
    std::mutex m;
    std::string logs;
    const int sink = log::add_sink([&](const log::Record& r) {
        std::lock_guard lock(m);
        logs += r.message + "\n";
    });
    w1d::FakeServer http_fake;
    std::atomic<int> opens{0};
    http_fake.router->post("/apps.connections.open", [&](http::Request&) {
        ++opens;
        return http::Response::json(Json{{"ok", false}, {"error", "invalid_auth"}});
    });
    http_fake.start();
    SocketModeDeps deps;
    deps.appToken = "xapp-secret-should-not-leak";
    deps.webApi.base_url = http_fake.base;
    deps.webApi.allow_private_network = true;
    deps.reconnectBase = std::chrono::milliseconds(5);
    deps.reconnectMax = std::chrono::milliseconds(10);
    deps.maxConsecutiveFailures = 3;
    auto handle = start_socket_mode(deps);
    for (int i = 0; i < 200 && handle->running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!handle->running());
    CHECK_EQ(opens.load(), 3);
    handle->stop();
    log::remove_sink(sink);
    CHECK(!str::contains(logs, "xapp-secret-should-not-leak"));
    CHECK(str::contains(logs, "invalid_auth"));
}

DI_TEST_MAIN()
