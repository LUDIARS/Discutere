#pragma once
// HTTP 要求の 1 回の試行 (http_client.cpp の内部部品)。
// 平文 (http) と TLS (https) を別の翻訳単位に分ける: Praesidium の静的解析は 1 単位 60 秒が上限で、
// 両方の経路を 1 単位に置くとその上限に近づくため。
#include <chrono>
#include <string>

#include <boost/asio/io_context.hpp>

#include "base/url.hpp"
#include "infra/http_client.hpp"

namespace di::http::detail {

/** 試行の結果。done=false なら error に最初の失敗理由。 */
struct Attempt {
    bool done = false;
    std::string error;
    int status = 0;
    std::string reason;
    Headers headers;
    std::string body;
};

struct AttemptArgs {
    url::Url url;
    std::string method;
    std::string body;
    /** 別オリジンへのリダイレクト後は Authorization / Cookie を送らない。 */
    bool drop_credentials = false;
    std::chrono::steady_clock::time_point connect_deadline;
};

/** io を回している間に解決 → (SSRF 検査) → 接続 → 送信 → 受信 を進める。req と out は io が止まるまで生かす。 */
void start_plain_attempt(boost::asio::io_context& io, const ClientRequest& req, AttemptArgs args, Attempt& out);
/** start_plain_attempt に TLS (SNI・ピア検証・ホスト名検証) を足したもの。 */
void start_tls_attempt(boost::asio::io_context& io, const ClientRequest& req, AttemptArgs args, Attempt& out);

} // namespace di::http::detail
