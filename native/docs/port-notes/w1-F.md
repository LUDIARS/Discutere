# w1-F 移植メモ (画面・管理 API の基礎・可視化の基礎・認証ミドルウェア)

job `cpp-node-20261030`。golden は `npx tsx native/tests/golden/w1-F-golden.gen.ts`
(画面 `native/web/**` と `native/tests/fixtures/w1f/*.json` を書き出す) と
`npx tsx native/tests/golden/w1-F-nfkc-table.gen.ts` (`native/src/flow/user_voices/nfkc_table.inc`)。

## 1. 機能の対応表

| TS | C++ |
|---|---|
| src/middleware/auth.ts | native/src/middleware/auth.{hpp,cpp} (`user_context` / `require_role` / `is_loopback_request` / `get_user_*` / 共通の `require_admin`) |
| src/observability/console-capture.ts | native/src/observability/console_capture.{hpp,cpp} (`di::log::add_sink`) |
| src/observability/error-buffer.ts | native/src/observability/error_buffer.{hpp,cpp} |
| src/runtime-settings/store.ts | native/src/runtime_settings/store.{hpp,cpp} (+ js_value.{hpp,cpp}: Number/String/Boolean の変換) |
| src/secrets/youtube-env.ts | native/src/secrets/youtube_env.{hpp,cpp} |
| src/api/conclusion-publication-editor.ts | native/src/api/conclusion_publication_editor.{hpp,cpp} + native/web/api/conclusion-publication-editor.html |
| src/api/consensus-persona-aggregation.ts | native/src/api/consensus_persona_aggregation.{hpp,cpp} |
| src/api/dashboard-routes.ts | native/src/api/dashboard_routes.{hpp,cpp} + native/web/api/dashboard.html |
| src/api/errors-routes.ts | native/src/api/errors_routes.{hpp,cpp} |
| src/api/top-page-routes.ts | native/src/api/top_page_routes.{hpp,cpp} + native/web/api/top-page.html |
| src/api/tuning-routes.ts | native/src/api/tuning_routes.{hpp,cpp} + native/web/api/tuning.html (`set_runtime_settings` / `set_rule_reseeder` で後から注入) |
| src/flow/anatomia/client.ts | native/src/flow/anatomia/client.{hpp,cpp} |
| src/flow/anatomia/types.ts | native/src/flow/anatomia/types.hpp |
| src/flow/foresight/markdown.ts | native/src/flow/foresight/markdown.{hpp,cpp} |
| src/flow/foresight/prompts.ts | native/src/flow/foresight/prompts.{hpp,cpp} |
| src/flow/foresight/state.ts | native/src/flow/foresight/state.{hpp,cpp} |
| src/flow/user-voices/reference-policy.ts | native/src/flow/user_voices/reference_policy.hpp |
| src/flow/user-voices/steam-app.ts | native/src/flow/user_voices/steam_app.{hpp,cpp} (+ nfkc.{hpp,cpp}, nfkc_table.inc) |
| src/flow/web/page.ts | native/src/flow/web/page.{hpp,cpp} + native/web/flow/page.html |
| src/visualize/data-sources.ts | native/src/visualize/data_sources.{hpp,cpp} |
| src/visualize/learning-layers.ts | native/src/visualize/learning_layers.{hpp,cpp} |
| src/visualize/topic-opinions.ts | native/src/visualize/topic_opinions.{hpp,cpp} |
| src/visualize/wikilink.ts | native/src/visualize/wikilink.{hpp,cpp} |
| (補助) | native/src/api/web_page.hpp (埋め込み画面を引く)、native/src/visualize/locale_compare.{hpp,cpp} (localeCompare の近似) |

ルートは `di::http::Router` を返す関数 (`dashboard_routes()` / `errors_routes()` / `top_page_routes()` / `tuning_routes()`)。
index では TS と同じく `top_page_routes` を `/`、他を `/api` に mount し、`/api/*` に `user_context()` を掛ける。
画面はどれもサーバ側の値を埋め込まない静的な文字列だったので `{{名前}}` 置換は使っていない。

## 2. テストの対応表

| TS テスト | ctest |
|---|---|
| (auth の境界・Hono との突き合わせ, golden) | middleware.auth |
| (console-capture / error-buffer / errors-routes, golden) | observability.error_buffer |
| tests/runtime-settings/runtime-settings.test.ts (applyRuleInstructionOverrides は別担当の debate-rules) | runtime_settings.store |
| tests/secrets/tuning-secrets.test.ts | runtime_settings.store (UI にキー入力が無い / secrets 系経路は 404) |
| tests/secrets/youtube-env.test.ts (package.json の scripts 検査は Node 側の話なので除く) | secrets.youtube_env |
| tests/api/consensus-persona-aggregation.test.ts | api.web_api |
| (画面のバイト一致・配信) | api.web_api |
| tests/flow/foresight.test.ts (prompt / markdown / state の部分。camps / phase / dispatch は別担当) | flow.foresight |
| tests/flow/user-voices.test.ts (Steam の部分) | flow.steam_app |
| tests/flow/anatomia-client.test.ts | flow.anatomia_client |
| tests/visualize/wikilink.test.ts / data-sources.test.ts / topic-opinions.test.ts / learning-layers.test.ts | visualize.visualize |

## 3. 安全の対応表

| 防御 / 入力経路 | C++ |
|---|---|
| userContext: X-User-Id があればそれ (role は X-User-Role、無ければ general)、無ければ loopback (127.0.0.1 / ::1 / ::ffff:127.0.0.1) は loopback-admin、他は anonymous | 同じ。空のヘッダ値は無いのと同じ (Hono と同じ)。golden で 11 アドレス × 8 ヘッダ組を突き合わせ、`::ffff:127.0.0.1`・`::1`・LAN・`127.0.0.2`・`0:0:0:0:0:0:0:1`・空・`localhost`・role だけの偽装を境界テスト |
| requireRole: context の role だけを見る (ヘッダは見ない) | 同じ (テストあり) |
| dashboard / errors の admin guard 401 / 403 | `middleware::require_admin` で同じ文言・status |
| tuning は guard 無し (loopback 運用前提) | TS と同じく付けない (強めない) |
| YouTube キーは env のみ・存在の有無だけ返す・更新 API なし | 同じ (secrets 系経路は 404 のまま) |
| tuning の HTTP 本文 | Router 既定の 1MiB 上限 + `parse_json` の上限 (壊れた・大きすぎる・深すぎる本文は TS の `.catch(() => ({}))` と同じく `{}`) |
| runtime_settings の DB 値 (override JSON) | 256KiB / 1 万要素 / 深さ 16 の上限で読む。超過・破損は既定値 |
| error-buffer | 件数 200 (TS と同じ) + 1 件の message を 64Ki (UTF-16 単位) で切る (新規)。mutex で守る |
| Steam (外部 HTTP) | 期限 15 秒、応答 2MiB、`allow_private_network=false` (SSRF 検査)、JSON は 2MiB / 20 万要素 / 深さ 32 |
| Anatomia CLI (子プロセス) | `proc::run` の引数配列 (シェル無し)、期限 (既定 180 秒)、標準出力 32MiB (TS の maxBuffer)、JSON 32MiB / 200 万要素 / 深さ 64 |
| affect 語彙ファイル | 4MiB 上限で読み、JSON は 20 万要素 / 深さ 32 |
| KG (SQLite) の読み出し | `sql::Database::all` の行数・バイト数の上限 (土台の既定) |
| wikilink / foresight の見出し除去 / console-capture の source 推測 | 正規表現を使わない線形の走査 (巨大・病的入力のテストあり) |

## 4. 移していないもの・挙動の差

- **NFKC と toLowerCase は近似** (steam-app の `normalizeGameName`)。ICU を入れないため、Node から書き出した
  1 文字ごとの NFKC 表 (4964 件) と「基底 + 結合文字」の合成表 (1203 組) を使う。結合文字の並べ替えとハングル字母の合成は
  しない。小文字化は ASCII / Latin-1 / Latin 拡張 A・追加 / ギリシャ (語末シグマの近似) / キリル / 全角英字のみ。
  golden の 31 件 (全角・半角カナ・ローマ数字・丸数字・合字・アクセント・トルコ語 İ・ギリシャ語) は一致。
- **localeCompare は近似** (consensus の displayName 並べ、learning-layers の affect key の同点並べ)。CLDR root の照合を
  3 段で近似 (記号の順は Node から取った表)。golden の 61 語は Node と同じ順。表に無い記号・ラテン拡張・他の文字の細かい順は ICU と違いうる (表示順だけに影響)。
- console-capture: TS は `console.error(a, b, …)` の引数を連結していた。C++ は log の 1 本の message を使う。
  先頭引数が Error オブジェクトのとき source が "console" になる、という区別は無い。`log::fatal` は error として積む。
- foresight の `contextBlock`: JS の slice がサロゲート対の途中で切ると孤立サロゲートになる (UTF-8 で表せない)。C++ は `str::js_slice` の扱いに従う。golden はこの場合を除いた。
- wikilink `normalizeType("constructor")` 等: TS は Object.prototype の関数を返していた (バグ)。C++ は nullopt。
- steam-app: `typeof id === "number"` でも小数の appId は受けない (Steam に存在しない)。`items` が配列でないときは TS では TypeError、C++ は「見つからない」。
- tuning: JSON 本文が `null` / `text` が文字列以外のときの TS の TypeError (Hono が 500 "Internal Server Error") は、C++ でも明示的に同じ 500 を返す。
- 可視化のスナップショット (data-sources / topic-opinions / learning-layers) は型付き struct ではなく TS と同じ形の `Json` を返す (ルートでそのまま出すだけのため)。
- learning-layers の語彙ファイルの場所はオプション `vocabularyPath` で差し替え可能にした (既定は TS と同じ `path.resolve("data/affects/vocabulary.json")`)。
- dashboard の HTML 内のブラウザ JS が呼ぶ他の管理 API (`/api/admin/status` 等) は別担当。

## 5. 土台に欲しいもの / 人間の判断が要ること

- 土台に `locale_compare` (ICU 互換の照合) と `normalize_nfkc` / `to_lower` (Unicode 全体) があると、表示順・ゲーム名照合を
  モジュールごとに近似しなくて済む。いまは `visualize/locale_compare` と `flow/user_voices/nfkc` に局所的に置いた
  (他担当で localeCompare / NFKC が要るならここを使うか土台へ移す)。
- `http::Headers::get` は同名ヘッダの最初の値を返す。Node は同名の X-User-Id を ", " で連結していた。admin ヘッダを
  2 つ送るような要求での差 (権限は広がらない: 連結値は "admin" にならない) を許容してよいか。
- tuning ルートは TS どおり認可なし (loopback 運用前提)。サーバを loopback 以外に bind する運用があるなら guard を足すかは判断が要る。
