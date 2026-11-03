#pragma once
// src/flow/web/routes.ts の移植: 議論フロー WebUI ルート (t7-entrypoints.md 経路 A)。
//
//   GET    /flow                                   1 ページ UI (native/web/flow/page.html)
//   POST   /api/flow/start                         フロー起動 (theme, flow, tags) → dispatch
//   GET    /api/flow/sessions                      議論一覧 (?state= &scope= &limit= &offset=)
//   GET    /api/flow/:session/paper                ペーパー編集草案の取得 (ready までポーリング)
//   POST   /api/flow/:session/paper/form/apply     固定フォームの反映
//   POST   /api/flow/:session/paper/debatability/check
//   POST   /api/flow/:session/paper/mechanics/check
//   POST   /api/flow/:session/paper/fix-suggestion/apply
//   POST   /api/flow/:session/paper/edit           410 (廃止)
//   POST   /api/flow/:session/paper/block/review   1 ブロックの LLM 改稿提案
//   POST   /api/flow/:session/paper/block/apply    ブロックの改稿/手編集を適用
//   POST   /api/flow/:session/paper/crawl          根拠 (外部の声) を集めて段落を提案 / 挿入
//   POST   /api/flow/:session/paper/revert         1 手前の本文に戻す
//   POST   /api/flow/:session/paper/approve        承認して議論を開始
//   POST   /api/flow/:session/say                  壁打ちのユーザ発話投入
//   DELETE /api/flow/:session                      議論の削除 (進行中なら止める)
//   GET    /api/flow/:session/status               発話 + 結論をポーリング取得 (?since=<ms>)
//
// 認証は他の HTTP UI と同じく loopback 信頼。進行・発話・結論は flow_* テーブルに永続され、
// ブラウザがポーリングで取得する (web-chat と同じ思想)。
//
// C++ 版の並行の扱い:
//   - 議論の実行・草案の構築は background_executor() に積んで即応答する (TS の void run)。
//     キューが満杯なら 503 {ok:false, error} で断る (積まなかった仕事の状態は残さない)。
//   - モジュールの状態 (TS の module-level Map) は 1 つの mutex で守る。LLM の待ちは mutex の外で行う。
//   - 議論ごとに FlowRunState を持ち、DELETE で進行中の議論を止める (request_stop)。
//   - 各ルートに本文上限 (RouteOptions.max_body) と JSON の上限を課す。
#include <chrono>
#include <cstddef>
#include <vector>

#include "base/json.hpp"
#include "flow/learning.hpp"
#include "flow/web/flow_prep.hpp"
#include "infra/http.hpp"

namespace di::flow::web {

/** /api/flow/start の本文上限 (仕様書の貼付本文 specText を含むため大きめ)。 */
inline constexpr std::size_t kFlowStartMaxBody = 8u << 20;
/** /paper/approve の本文上限 (直接編集したペーパー全体を含む)。 */
inline constexpr std::size_t kFlowApproveMaxBody = 4u << 20;
/** 本文 md を運ぶ編集系 (form/apply・block/apply・fix-suggestion/apply)。 */
inline constexpr std::size_t kFlowEditMaxBody = 2u << 20;
/** それ以外の小さな操作 (say・block/review・crawl・check 系・revert・edit)。 */
inline constexpr std::size_t kFlowSmallMaxBody = 256u << 10;

/** 起動後に deps を差し替える (TS の setFlowWebDeps)。スレッド安全。 */
void set_flow_web_deps(FlowWebDeps deps);

http::Router flow_routes();

/** 合成した仮説ラベル付きの声を多様性で選ぶ (TS の normalizeSyntheticLearningOpinions)。 */
std::vector<LearningOpinion> normalize_synthetic_learning_opinions(const Json& raw_items, double posted_at);

/** テスト用: 登録セッション・タイマーをクリアする (TS の _resetFlowWeb)。deps は残す。 */
void reset_flow_web_for_test();
/** テスト用: このモジュールが積んだ背景の仕事がすべて終わるまで待つ (true = 終わった)。 */
bool wait_flow_web_idle_for_test(std::chrono::milliseconds timeout);

} // namespace di::flow::web
