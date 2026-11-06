#include "infra/ws_client.hpp"

#include <utility>

#include "base/url.hpp"
#include "infra/ws_client_detail.hpp"

namespace di::ws {

std::shared_ptr<Client> Client::connect(const Options& options, OnMessage on_message, OnClose on_close, std::string* error) {
    auto u = url::parse(options.url);
    if (!u || (u->scheme != "ws" && u->scheme != "wss")) {
        if (error) *error = "invalid websocket url";
        return nullptr;
    }
    if (u->scheme == "wss") return detail::connect_tls(options, *u, std::move(on_message), std::move(on_close), error);
    return detail::connect_plain(options, *u, std::move(on_message), std::move(on_close), error);
}

} // namespace di::ws
