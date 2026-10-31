#pragma once
// 外部への HTTP/HTTPS 要求 (fetch の置き換え)。同期 API: 呼んだスレッドで待つ。
// - 期限: 接続 (connect_timeout) と全体 (timeout)。
// - 応答本文は max_response_bytes まで (lorica::input_budget で課金)。超えたら失敗。
// - 同時実行はプロセス全体で 32 本まで。空きを connect_timeout まで待ち、待てなければ失敗 (待機→拒否)。
// - TLS はピア検証 + ホスト名検証あり (OpenSSL の既定の信頼ストア。Windows はシステムの ROOT ストア)。
// - SSRF 対策: allow_private_network=false (既定) のとき、名前解決した IP がすべて公開アドレスで
//   あることを確かめ、その IP に固定して接続する。loopback 等の設定済みローカルサービスへは true で呼ぶ。
// - リダイレクトは fetch と同じく既定で追う (最大 max_redirects)。追うたびに SSRF 検査をやり直し、
//   別オリジンへは Authorization / Cookie を送らない。
// - Accept-Encoding は送らない (解凍処理を持たない)。
#include <chrono>
#include <cstddef>
#include <string>

#include "infra/http.hpp"

namespace di::http {

struct ClientRequest {
    std::string method = "GET";
    std::string url;
    Headers headers;
    std::string body;
    std::chrono::milliseconds timeout{30'000};
    std::chrono::milliseconds connect_timeout{10'000};
    std::size_t max_response_bytes = 16u << 20;
    bool allow_private_network = false;
    int max_redirects = 5;
};

struct ClientResponse {
    /** 通信自体が成立したか (false なら error に理由、status は 0)。 */
    bool transport_ok = false;
    std::string error;
    int status = 0;
    Headers headers;
    std::string body;
    std::string final_url;
    bool ok() const { return transport_ok && status >= 200 && status < 300; }
};

ClientResponse fetch(const ClientRequest& request);

/** IP アドレス文字列が公開アドレスか (private / loopback / link-local / CGNAT / multicast 等は false)。 */
bool is_public_address(const std::string& ip);

} // namespace di::http
