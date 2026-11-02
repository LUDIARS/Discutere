#pragma once
// src/api/glab-review-trend-routes.ts の移植: GLAB には匿名化済みのゲーム別集計だけを公開する。
// GET /integrations/glab/review-trends ("/api" に mount する前提)。
//   - Cache-Control: private, no-store を常に付ける。
//   - token (DISCUTERE_GLAB_REVIEW_TRENDS_TOKEN) が 32 文字未満/未設定なら 503 {"ok":false,"error":"review_trends_unavailable"}。
//   - Authorization: Bearer <1 語> を定数時間で比較 (違えば 401 {"ok":false,"error":"unauthorized"})。
//   - 集計の失敗は 503 (内部の詳細は出さない)。
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "infra/http.hpp"
#include "integrations/glab_review_trends.hpp"

namespace di::api {

inline constexpr std::size_t kGlabReviewTrendMinTokenLength = 32;

using GlabTrendLoader = std::function<std::vector<integrations::GameReviewTrend>()>;
using GlabTokenLoader = std::function<std::string()>;

/** 既定は load_stored_steam_review_trends と env の token。 */
http::Router glab_review_trend_routes(GlabTrendLoader load_trends = {}, GlabTokenLoader token = {});

} // namespace di::api
