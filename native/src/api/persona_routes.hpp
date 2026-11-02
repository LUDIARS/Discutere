#pragma once
// @spec インポート (Vo → Di)
// src/api/persona-routes.ts の移植: 外部由来の匿名ペルソナの管理 (loopback 前提の admin 面、"/api" に mount)。
//   GET  /admin/personas              画面 (native/web/api/personas.html)
//   GET  /admin/personas/data         自動採用の設定と件数
//   PUT  /admin/personas/auto-adopt   flow.autoAdoptOnCrawl を config ファイルに書き戻す
//   POST /admin/personas/import       Voluptas の仮名 export (JSON 配列 / {personas} / NDJSON) を取り込む
//   POST /admin/personas/adopt        公開レビュー話者の採用 (既定 dry-run)
// TS と同じく役割の検査は無い (index で loopback に閉じる前提)。
#include <cstddef>
#include <functional>

#include "flow/persona_adopt_runner.hpp"
#include "infra/http.hpp"

namespace di::api {

/** import の本文上限 (persona_document_json_limits と同じ 16MiB)。 */
inline constexpr std::size_t kPersonaImportMaxBody = 16u << 20;

struct PersonaRouteDeps {
    /** 採用ランナー (既定 flow::run_adopt_from_kg)。 */
    std::function<flow::AdoptRunSummary(const flow::AdoptRunOptions&)> runAdopt;
};

http::Router persona_routes(PersonaRouteDeps deps = {});

} // namespace di::api
