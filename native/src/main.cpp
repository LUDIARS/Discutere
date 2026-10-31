// discutere-server のエントリ (src/index.ts の移植は最後の段階で行う。今は /health だけ)。
#include <csignal>
#include <iostream>

#include "base/env.hpp"
#include "base/executor.hpp"
#include "base/log.hpp"
#include "base/strings.hpp"
#include "infra/http_server.hpp"

int main() {
    using namespace di;
    auto router = std::make_shared<http::Router>();
    router->get("/health", [](http::Request&) { return http::Response::json(Json{{"status", "ok"}, {"service", "discutere"}}); });
    http::ServerOptions options;
    options.host = env::get_nonempty("BACKEND_HOST").value_or("127.0.0.1");
    options.port = static_cast<std::uint16_t>(str::parse_int(env::get_nonempty("BACKEND_PORT").value_or("3110")).value_or(3110));
    http::Server server(options, router);
    server.start();
    log::info("[discutere] HTTP listening on " + options.host + ":" + std::to_string(server.bound_port()));
    StopSignal stop;
    static StopSignal* stop_ptr = &stop;
    std::signal(SIGINT, [](int) { stop_ptr->request_stop(); });
    std::signal(SIGTERM, [](int) { stop_ptr->request_stop(); });
    while (!stop.wait_for(std::chrono::milliseconds(500))) {
    }
    server.stop();
    return 0;
}
