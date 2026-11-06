#include "infra/ws_client_detail.hpp"
#include "infra/ws_client_impl.hpp"

namespace di::ws::detail {

std::shared_ptr<Client> connect_tls(const Options& options, const url::Url& u, OnMessage on_message, OnClose on_close,
                                    std::string* error) {
    return connect_with<boost::beast::ssl_stream<boost::beast::tcp_stream>>(options, u, std::move(on_message), std::move(on_close),
                                                                            error);
}

} // namespace di::ws::detail
