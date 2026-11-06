#include "infra/http_attempt_op.hpp"

namespace di::http::detail {

void start_tls_attempt(boost::asio::io_context& io, const ClientRequest& req, AttemptArgs args, Attempt& out) {
    std::make_shared<AttemptOp<true>>(io, req, std::move(args), out)->start();
}

} // namespace di::http::detail
