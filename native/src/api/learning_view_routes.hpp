#pragma once
// 学習ビュー (src/api/learning-view-routes.ts の移植)。index では app 直下 ("/") に mount する。
//
//   GET  /learning                               画面 (native/web/api/learning.html)
//   GET  /learning/gap, /learning/gap/detail     Gap率 (事前ビルドの learning-cache から配信)
//   GET  /learning/sources                       データソース構成 (learning-cache)
//   GET  /learning/conclusions                   結論一覧。conclusion_cache (discutere.db) だけを読む = KG を開かない。
//                                                キャッシュが空のときだけ live で KG + flow をマージする
//   GET  /learning/conclusion?gap=               1 件の論述データ (gap="flow:<sessionId>" は新フロー)
//   GET  /learning/conclusion/export?gap=        議論 md のダウンロード
//   POST /learning/conclusion/export/fundamentum 議論を Fundamentum へ個別 export
//   GET  /learning/data?layer=                   学習レイヤのスナップショット (KG)
//
// 外部の声の個人アンカーは conclusions の serializer が仮名化する (§6)。出所メタは attribution store から付ける。
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "api/consensus_routes.hpp"
#include "base/sqlite.hpp"
#include "crawler/sources/attribution_store.hpp"
#include "infra/http.hpp"

namespace di::api {

struct LearningViewDeps {
    /** active KG を開く (既定 open_active_core)。 */
    CoreOpener openCore;
    /** flow DB (discutere.db。既定 flow::db::get_flow_db)。 */
    std::function<sql::DatabasePtr()> flowDb;
    /** 出所メタの store (既定 open_attribution_store())。 */
    std::function<std::unique_ptr<crawler::sources::AttributionStore>()> openAttribution;
    /** learning-cache の場所 (既定 default_learning_cache_path)。 */
    std::optional<std::string> learningCachePath;
};

http::Router learning_view_routes(LearningViewDeps deps = {});

} // namespace di::api
