#pragma once
// src/api/datasource-routes.ts の移植: データ調整 UX (admin) — spec/feature/crawler/EXTERNAL-SOURCES.md §13。"/api" に mount。
//   GET  /admin/datasources                         active KG + 宣言済み KG + source 別件数
//   GET  /admin/datasources/:source/utterances      当該 source の取り込み発話一覧 (出所付き)
//   POST /admin/datasources/exclude                 発話/source 単位の除外 (学習・議論から外す)
//   POST /admin/datasources/active-kg               次に開く KG を config に書き戻す (再起動で反映)
// 全ルート admin role 必須 (401/403)。個人は §6 露出 serializer (masked_persona_label) でマスクし、生 ID は admin のみ。
// 除外は既存 noise 除去 (utterance_exclusions) + .ingested 台帳に寄せる。
#include "infra/http.hpp"

namespace di::api {

http::Router datasource_routes();

} // namespace di::api
