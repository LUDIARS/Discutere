#pragma once
// infra 内部専用: TLS クライアントの共有コンテキスト (Boost.Asio の型を出すので infra の .cpp からだけ使う)。
#include <boost/asio/ssl.hpp>

namespace di::infra {

/** TLS 1.2 以上・ピア検証あり。信頼ストアは OpenSSL の既定 (SSL_CERT_FILE / SSL_CERT_DIR も効く)、
 *  Windows ではシステムの ROOT ストアを読み込む。 */
boost::asio::ssl::context& tls_client_context();

} // namespace di::infra
