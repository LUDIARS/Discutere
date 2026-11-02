#pragma once
// 使い方ガイド (GET /guide) — Discord フォーラム / チャンネルの使い方とルール (src/api/guide-routes.ts)。
// 画面は移植しない: Node の出力を native/web/api/guide.html に置き、config の値を入れる位置に {{名前}} の印を置いた。
// ここでは config.discord の値を HTML エスケープして印を置き換えるだけ。"/" に mount する前提。loopback 前提で guard 無し (TS と同じ)。
#include <string>

#include "config.hpp"
#include "infra/http.hpp"

namespace di::api {

using DiscordConfig = decltype(config::DiscutereConfig::discord);

/** TS の esc() (& < > " の 4 文字。' はエスケープしない)。 */
std::string guide_escape(const std::string& s);
/** ガイドの HTML を組み立てる (テスト用に公開)。 */
std::string render_guide_html(const DiscordConfig& discord);

/** 使い方ガイドのルートを作る。HTML は作成時に 1 度だけ組み立てる (TS と同じ)。 */
http::Router create_guide_routes(const DiscordConfig& discord);

} // namespace di::api
