# w3-O port-notes — ペーパーレビュー・各種 API ルート・軽量 Web チャットの部品

job `cpp-node-20261030`。TS 14 ファイル (2,366 行) を C++ に移した。golden 生成は
`npx tsx native/tests/golden/w3-O-golden.gen.ts` (→ `native/tests/fixtures/w3o/golden.json` と画面 2 枚)。

## 1. 機能の対応表 (TS → C++)

| TS | C++ | 備考 |
|---|---|---|
| `src/flow/paper-review.ts` | `native/src/flow/paper_review.{hpp,cpp}` | `build_paper_draft` / `assess_paper_understanding` / `apply_paper_edit` / `coerce_paper_draft` / `render_paper_review` / `is_approval_text` / `is_revert_text` / `review_block` / `gather_evidence` / `merge_mechanics` / `with_derived_body` / `with_derived_structure`。`PaperDraft` は `PaperContent` を継承 + `bodyMd`。省略可能な LLM は `LLMClient*` (nullptr = 無し) |
| `src/flow/paper-review-enrich.ts` | `native/src/flow/paper_review_enrich.{hpp,cpp}` | `build_intent_instruction`。crawl / investigate は注入境界 (`PaperIntentCrawlFn` は取り込み件数を返す) |
| `src/flow/persona-adopt-runner.ts` | `native/src/flow/persona_adopt_runner.{hpp,cpp}` | `run_adopt_from_kg`。テスト用に `collect_speaker_opinions` / `adopt_from_speakers` に分けた |
| `src/api/persona-routes.ts` | `native/src/api/persona_routes.{hpp,cpp}` + `native/web/api/personas.html` | `persona_routes(PersonaRouteDeps)` (採用ランナーを差し替え可) |
| `src/api/noise-routes.ts` | `native/src/api/noise_routes.{hpp,cpp}` + `native/web/api/noise.html` | `noise_routes()` |
| `src/integrations/glab-review-trends.ts` | `native/src/integrations/glab_review_trends.{hpp,cpp}` | `read_stored_steam_review_signals` (attribution の列挙を関数で受ける版と store 版) / `analyze_stored_review_trends` / `load_stored_steam_review_trends` / `steam_app_id_from_source_url` |
| `src/api/glab-review-trend-routes.ts` | `native/src/api/glab_review_trend_routes.{hpp,cpp}` | `glab_review_trend_routes(load, token)` |
| `src/api/persona-bridge-routes.ts` | `native/src/api/persona_bridge_routes.{hpp,cpp}` | `persona_bridge_routes(PersonaBridgeRouteDeps)`。認証は w2-J の `persona_bridge::authorize_bridge_request` |
| `src/web-chat/session.ts` | `native/src/web_chat/session.{hpp,cpp}` | `web_scene` / `sanitize_room_id` / `ensure_web_session` / `web_discussion_exists` |
| `src/web-chat/transcript.ts` | `native/src/web_chat/transcript.{hpp,cpp}` | `read_room_transcript` (Core 版と `sql::Database&` 版) |
| `src/api/datasource-routes.ts` | `native/src/api/datasource_routes.{hpp,cpp}` | `datasource_routes()` |
| `src/api/kg-migration-routes.ts` | `native/src/api/kg_migration_routes.{hpp,cpp}` | `kg_migration_routes(KgMigrationRouteDeps)` (serveToken / export を差し替え可) |
| `src/api/queue-routes.ts` | `native/src/api/queue_routes.{hpp,cpp}` | `queue_routes()` |
| `src/kg-sync/scheduler.ts` | `native/src/kg_sync/scheduler.{hpp,cpp}` | `start_kg_sync_scheduler(config, run_once)` → 停止関数 |
| (共通補助) | `native/src/api/config_file_patch.{hpp,cpp}` | persona-routes / datasource-routes の config 書き戻し (上限つき読み込み・一時ファイル経由の置き換え・mutex) |

ルートはどれも `"/api"` に mount する前提 (TS の `app.route("/api", ...)`)。`main.cpp` への配線は担当外 (index の移植担当)。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| `tests/flow/paper-review.test.ts` (共有コア部分) | `flow.paper_review` |
| `tests/flow/paper-review-discussion-mechanics.test.ts` | `flow.paper_review` |
| `tests/flow/debatability.test.ts` の buildPaperDraft (`# 論点` 前倒し) | `flow.paper_review` (golden の gate / gate-degraded) |
| `tests/flow/anatomia-refine.test.ts` の mergeMechanics | `flow.paper_review` (golden の merge) |
| (TS テスト無し) paper-review-enrich | `flow.paper_review` (golden の enrich) |
| (TS テスト無し) persona-adopt-runner | `flow.persona_adopt_runner` |
| `tests/flow/glab-review-trends.test.ts` | `integrations.glab_review_trends` |
| `tests/flow/persona-bridge.test.ts` の route 部分 | `api.persona_bridge_routes` |
| `tests/flow/persona-routes.test.ts` | `api.w3o_admin_routes` |
| (TS テスト無し) noise / datasource / queue / kg-migration | `api.w3o_admin_routes` |
| (TS テスト無し) web-chat session / transcript | `web_chat.web_chat` |
| (TS テスト無し) kg-sync scheduler | `kg_sync.scheduler` |

移していない TS テストの部分: `paper-review.test.ts` の director の paperOverride / config の env 配線 /
`discord-live.handlePaperReviewApproval` (担当外のモジュール)。`persona-bridge.test.ts` の descriptor・NDJSON pull・
utterance-export・replay store (w2-J / w2-H の担当分で移植済み)。

ctest の名前は `<ディレクトリ>.<名前>` (tests/CMakeLists.txt の規則)。

## 3. 安全の対応表

| 経路 | 移した防御 | 追加した上限・課金 |
|---|---|---|
| LLM 応答 (understanding / edit / review_block) | 壊れた・形の違う応答は元のドラフト/決定論に倒す (TS と同じ) | `parse_json(..., llm_json_limits())` (4MiB・20 万要素・深さ 64)。上限ちょうど / +1 / 深すぎ / 巨大をテスト |
| `/integrations/glab/review-trends` | 32 文字未満は 503・`Bearer <1 語>` を `constant_time_equal`・失敗は 503 で詳細を出さない・`private, no-store` | ― (トークン長は TS と同じ UTF-16 長) |
| `/persona-bridge/utterances` | service token / 32 文字以上の固定トークン (bridge_auth)・公開鍵未設定 503・snowflake 検証・limit 1..1000・Ed25519 アサーション (1 回限り)・読み出し失敗は 500 定型文 | 結果は limit+1 件だけ読む (TS と同じ) |
| `/kg/migrations` | serveToken 設定時のみ token 必須 (403) | 比較を TS の `===` から `constant_time_equal` に強めた |
| noise / datasource / queue | admin 判定 (userId 無し 401 / role≠admin 403、`middleware::require_admin`) | DB 一覧の `all()` に行数・バイト数の上限 (`kNoiseListLimits` 等) |
| datasource 発話一覧 | §6: 個人は `masked_persona_label` で `論者#xxxx`、出所は開示、生 ID は admin のみ | limit を 1..500 に (数でない値は 200) |
| persona import | 本文は JSON / NDJSON (w2-H の `parse_persona_document` の上限 16MiB) | ルートの本文上限 16MiB (ちょうど / +1 = 413 をテスト) |
| config 書き戻し (auto-adopt / active-kg) | ― | 読み込み上限 (`kMaxConfigFileBytes`)・壊れた JSON / object でない場合は書かずに 500・一時ファイル経由の置き換え・mutex |
| web-chat transcript | 個人データを保存しない (author は表示名のみ・人間発話は入口 session 由来だけ) | `all()` に `kTranscriptReadLimits` (10 万行・64MiB) |
| persona-adopt-runner | 生 authorId を返さない (採用側で仮名化) | KG 読み出しに `kAdoptKgReadLimits` |
| kg-sync scheduler | pull 本体 (w2-J/w1 の `pull_shared_kg`: http/https のみ・期限・応答上限) | 同時実行 1 本 (前回が終わっていなければ飛ばす)・executor 満杯なら飛ばす・ログの URL からクエリを落とす |
| 機密/内部タグ | `gate_external_voices` で蓄積済みの外部の声もペーパーに載せない | テスト「機密/内部タグでは…」 |

## 4. 移していないもの・挙動の差

- `get_config()` のキャッシュを捨てない: TS の persona-routes は config 書き込み後に `_resetConfig()` する。C++ の
  `get_config()` は参照を返すため実行中に捨てると他スレッドの参照が宙に浮く。応答の `autoAdoptOnCrawl` は
  `load_config()` で読み直した値を返す (画面の表示は TS と同じ)。**ただしクロール後の自動採用など
  `get_config()` を読む他の箇所は再起動まで古い値のまま** (TS は即時反映)。→ 5 の土台への要望。
- `persona_bridge` の `since`: TS は `Date.parse` (処理系依存の非 ISO 形式も通る)。C++ は `time::parse_iso` (ISO 8601) だけ。
  数字列 (epoch ms) と ISO は同じ。
- glab の並び: 同数・同時刻のときの `localeCompare` (ICU の照合) は w1-F の `visualize::locale_compare` (近似) で比べる。
- glab の URL 解析: `url::parse` は userinfo つき URL を拒否する (WHATWG URL は受ける)。Steam collector が残す URL には付かない。
- datasource の `limit` が数でないとき: TS は NaN を SQL に渡していた (実質エラー)。C++ は既定 200。
- noise / datasource / persona の本文が JSON の `null` のとき TS は TypeError (500)。C++ も 500 "Internal Server Error"。
  `utteranceId` が文字列以外 (数値など) のとき TS は SQLite にそのまま束縛していた。C++ は「無い」(400) とする。
- `assess_paper_understanding` のプロンプトに載せるメカニクスの JSON のキー順は C++ の `to_json(MechanicSummary)` の順
  (name, description, source, intended_*)。TS は生成元のオブジェクトの順なので、LLM 増補・編集由来のメカニクスでは
  キーの並びだけ違うことがある (data/games 由来は同じ。golden で確認)。
- config ファイルが JSON の配列など object 以外のとき TS は書き込みを黙って無視していた。C++ は 500 で書かない。
- kg-sync scheduler のログ: TS は `sourceUrl` をそのまま出す。C++ はクエリを落とす (token が載り得るため)。
- `review_block` / `gather_evidence` の `llm` / `listExternalVoices` は TS では必須引数。C++ は `review_block(llm, args)` で
  llm を先頭引数にした (`LLMClient&`)。`gather_evidence` は `listExternalVoices` が空なら声 0 件として扱う。

## 5. 土台に欲しいもの / 人間の判断が要ること

- **設定の再読み込み**: `config::get_config()` が `std::shared_ptr<const DiscutereConfig>` のスナップショットを返し、
  `config::reload()` で差し替えられる API が欲しい (TS の `_resetConfig()` を本番経路で使っている箇所があるため:
  persona-routes の autoAdoptOnCrawl)。それまでは自動採用の切替は再起動で反映、と運用に書く必要がある (人間の判断)。
- **index (main.cpp) への配線**: この担当のルート (`glab_review_trend_routes` / `queue_routes` / `noise_routes` /
  `datasource_routes` / `persona_routes` / `persona_bridge_routes` / `kg_migration_routes`) と
  `start_kg_sync_scheduler` は index の移植担当が `"/api"` に mount / 起動時に呼ぶ。persona-routes は TS と同じく
  役割検査が無い (loopback に閉じる前提) ので、mount 側の loopback ガードを外さないこと。
- `visualize::locale_compare` (w1-F の ICU 近似) を glab の並びにも使った。汎用の照合なので `base/strings` に移すと良い。
