#pragma once
// ws_client.cpp の内部部品。平文 (ws) と TLS (wss) を別の翻訳単位に分ける: Praesidium の静的解析は
// 1 単位 60 秒が上限で、両方の経路を 1 単位に置くとその上限に届くため。
#include <memory>
#include <string>

#include "base/url.hpp"
#include "infra/ws_client.hpp"

namespace di::ws::detail {

std::shared_ptr<Client> connect_plain(const Options& options, const url::Url& u, OnMessage on_message, OnClose on_close,
                                      std::string* error);
std::shared_ptr<Client> connect_tls(const Options& options, const url::Url& u, OnMessage on_message, OnClose on_close,
                                    std::string* error);

} // namespace di::ws::detail
