# port-notes w2-H: ペルソナの母集団・代理議論の束・Notion 読み込み等

job `cpp-node-20261030`。担当の TS 19 ファイルを C++ に移した (Notion の取得そのものは Canalis を呼ぶ Node サイドカー)。

## 1. 機能の対応表 (TS → C++)

| TS | C++ |
|---|---|
| `src/flow/persona-descriptor.ts` | `native/src/flow/persona_descriptor.{hpp,cpp}` |
| `src/flow/persona-pool.ts` | `native/src/flow/persona_pool.{hpp,cpp}` (`PoolPersona` と `to_json`、DB の JSON 列は上限つきで読む) |
| `src/flow/persona-adopt.ts` | `native/src/flow/persona_adopt.{hpp,cpp}` |
| `src/flow/persona-cast.ts` | `native/src/flow/persona_cast.{hpp,cpp}` |
| `src/flow/persona-import.ts` | `native/src/flow/persona_import.{hpp,cpp}` (正規表現は線形の検査関数) |
| `src/flow/proxy/bundle.ts` | `native/src/flow/proxy/bundle.{hpp,cpp}` |
| `src/flow/proxy/store.ts` | `native/src/flow/proxy/store.{hpp,cpp}` |
| `src/flow/proxy/cast.ts` | `native/src/flow/proxy/cast.{hpp,cpp}` (`ProxyRequestError`、DIRECTIVE / GENRE / LABEL の正規表現は線形の走査) |
| `src/flow/proxy/resolve.ts` | `native/src/flow/proxy/resolve.{hpp,cpp}` |
| `src/flow/proxy/stance.ts` | `native/src/flow/proxy/stance.{hpp,cpp}` (`judge_proxy_stance` は同期) |
| `src/flow/design-gap.ts` | `native/src/flow/design_gap.{hpp,cpp}` (gray-matter → `lib/front_matter`) |
| `src/flow/notion-link.ts` | `native/src/flow/notion_link.{hpp,cpp}` (URL 検出・深さ指示・md 連結) + `native/sidecar/canalis-crawl.ts` (Canalis の `NotionApiClient` / `crawlPage` / `crawlPublicPages`) |
| `src/flow/user-voices/request-client.ts` | `native/src/flow/user_voices/request_client.{hpp,cpp}` (`VoiceRequestFailure`、`configured_voice_request`) |
| `src/flow/user-voices/preparation.ts` | `native/src/flow/user_voices/preparation.{hpp,cpp}` (`VoiceRequestPort` は submit / status の関数の組) |
| `src/flow/llm-fallback.ts` | `native/src/flow/llm_fallback.{hpp,cpp}` |
| `src/flow/model-router.ts` | `native/src/flow/model_router.{hpp,cpp}` (codex は `persona_engine/llm/codex_cli` を外から渡す) |
| `src/flow/anatomia/refine.ts` | `native/src/flow/anatomia/refine.{hpp,cpp}` |
| `src/flow/anatomia/index.ts` | `native/src/flow/anatomia/index.{hpp,cpp}` |
| `src/flow/web/spec-extract-route.ts` | `native/src/flow/web/spec_extract_route.{hpp,cpp}` (`spec_extract_routes()` が `http::Router` を返す。multipart の読み取りはこのモジュール内) |

golden 生成: `npx tsx native/tests/golden/w2-H-flow.gen.ts` → `native/tests/fixtures/flow/w2h.json`。
サイドカーの取得手順の確認: `native/tests/golden/w2-H-canalis-crawl.check.ts` (ctest `flow.notion_link` が node で起動する)。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| `tests/flow/persona-pool.test.ts`, `tests/flow/persona-import.test.ts` (母集団推定を除く), `tests/flow/persona-state.test.ts` (buildFlowCast), `tests/flow/persona-bridge.test.ts` (descriptor) | `flow.persona_pool` |
| `tests/flow/proxy-debate.test.ts` (束・取込 (store 部分)・立場・指定の読み取りとキャスト・resolveProxyRun) | `flow.proxy_cast` |
| `tests/flow/improvement.test.ts` / `effect-predict.test.ts` / `vote-lenses.test.ts` (design-gap の部分) | `flow.design_gap` |
| `tests/flow/notion-link.test.ts` | `flow.notion_link` (C++ 部分 + 実サイドカーの入力検査 + `w2-H-canalis-crawl.check.ts` で本物の `crawlPage` と偽 Notion API による深さ 0/2/3・公開ページ切替) |
| `tests/flow/voice-request-client.test.ts`, `tests/flow/voice-preparation.test.ts` (再議論の部分を除く) | `flow.user_voices.voice_request` (偽 fetch + 偽 HTTP サーバ) |
| `tests/flow/model-roster.test.ts` (router 節) | `flow.llm_router` (FallbackLlm も) |
| `tests/flow/anatomia-refine.test.ts` (mergeMechanics を除く) | `flow.anatomia_refine` |
| (spec-extract-route はテスト無し) | `flow.web.spec_extract_route` |

ctest 全体: 85/85 通過 (ビルドディレクトリ `/home/user/build-w2-H`)。

## 3. 安全の対応表

| 防御 / 入力経路 | C++ |
|---|---|
| Voluptas 仮名 ID のみ受理 (`ext:voluptas:<16 hex>`)・20 次元 0..1・版・正準の嗜好軸・属性長 | 同じ検査と同じ文言 (golden で照合)。取込の 1 行の失敗で全体を落とさない |
| 露出名の仮名化 (`論者#xxxxxx` / `プレイヤー#xxxxxxxx`) | 同じ (`masked_persona_label` / SHA-256)。生 ID を名前に入れない |
| Voluptas profile が正本 (レビュー採用でベクトルを置き換えない) | 同じ |
| 期限切れの派生プロフィールの purge (プールを開くたび) | 同じ (`purge_history_personas`) |
| 代理議論: `機密`/`内部` で使わない・無効設定・指定無しは理由付きで拒否・評価前/失効/期限切れ/低信頼は出さない | 同じ |
| 代理の束: 数値を出さない・束に載った id だけを渡す | 同じ (golden で全文照合) |
| Voluptas 依頼: HTTPS (loopback のみ HTTP)・userinfo 拒否・health を先に確認・redirect を追わない・読み取り用固定トークンを使わない | 同じ。期限 health 5 秒 / 依頼 10 秒、応答 64KiB (lorica の課金つき `parse_json`)。token をログ・エラー文に出さない |
| model router: gpt-* の失敗を Claude で代弁しない | 同じ |
| Notion: token は Node 側だけ | C++ は token を持たない。サイドカーは入力 16KiB・URL を `isNotionUrl` で再検査・depth 0..5・maxPages 1..200 |
| 外部入力の上限 (lorica) | DB の JSON 列 (flow_persona 1MiB/10 万要素/深さ 16、pack_json 1MiB、voice payload 64KiB)、data/games のファイル (investigate と同じ 4MiB/64MiB)、LLM 応答 (Anatomia 1MiB/深さ 32)、サイドカー出力 32MiB、HTTP 本文 (`/api/spec/extract` 6MiB、`/api/spec/github/check` 64KiB)、multipart (部品 32・見出し 8KiB・`lorica::input_budget`)、一覧の行数 (`sql::QueryLimits`) |
| 正規表現 | すべて線形の走査。巨大入力 (100 万字級) のテストあり |

## 4. 移していないもの・挙動の差

- **Notion の取得** (Notion API / Playwright) は Node のサイドカー `native/sidecar/canalis-crawl.ts`。API の深さ辿り・公開ページ取得・warn の文言は移植前と同じ手順をサイドカー内で行う。
- Notion: 移植前は config の `maxDepth` / `maxPages` に上限が無かった。C++ は depth を 0..5、maxPages を 1..200 に丸める (本文の「深さ:N」は従来どおり 5 で頭打ち)。1 本文から展開するリンクは 20 件まで (超えたら warn)。サイドカーの期限は 60 秒 + 30 秒 × maxPages (上限 15 分)、移植前は全体の期限が無かった。
- Notion URL の検出: URL の読みは `di::url::parse` なので、WHATWG の URL が受ける userinfo つき (`https://a@www.notion.so/x`) や IDN を Notion URL とみなさない (移植前は通った)。
- `/api/spec/extract`: 移植前 (Hono) は本文の上限が無く、読み終えてから 5MB 超を 413 にしていた。C++ は本文を 5MB + 1MiB で打ち切る (それを超えるとサーバの汎用 413 で、本文の JSON 文言は付かない)。5MB〜6MB は移植前と同じ JSON の 413。multipart の `filename*` (RFC 5987) と文字コード指定は読まない (ブラウザの通常の送信では使われない)。
- `design_gap.load_current_vector`: game_vector の要素が数値でないファイルは使わない (移植前はそのまま返した)。
- `persona_pool`: DB の JSON 列で型の違う要素 (traits の数値等) は落とす (移植前はそのまま型を偽って返した)。
- `proxy/store`: pack_json は読むたびに契約 (`parse_persona_pack`) で検査し直し、通らなければ例外 (移植前は JSON.parse のみ)。
- `user_voices/preparation`: payload の未知のキーは読み捨てる。
- `persona-import.ts` / `proxy/store.ts` はファイルを読まない (入力は呼び出し側が上限つきで読んだ JSON / DB)。
- テストで移していないもの: `persona-import.test.ts` の母集団推定 (`persona-populations.ts`、担当外)、`voice-preparation.test.ts` の再議論 (`discord-flow-runs.ts`、担当外)、`anatomia-refine.test.ts` の `mergeMechanics` (`paper-review.ts`、担当外)、`proxy-debate.test.ts` の取込 (import.ts、CLI)・形成・評価・dialectic E2E (担当外)。

## 5. 土台に欲しいもの / 人間の判断が要ること

- **土台に欲しいもの**
  - `localeCompare` の近似 (`visualize/locale_compare.hpp`) を `base/strings` に (descriptor / import の並べ替えで使っている)。
  - `str::is_js_word_char` / 単語境界の検索 (`\b`) を `base/strings` に。
  - multipart/form-data の読み取りを infra に (今は `flow/web/spec_extract_route.cpp` 内の最小実装)。
  - `http::fetch` の応答上限超過を `transport_ok=false` と区別できる種別 (Voluptas 依頼では「down」扱いにしている)。
- **人間の判断が要ること**
  - Notion の maxPages 上限 200・リンク 20 件・サイドカー期限 15 分でよいか。
  - `canalis-crawl.ts` を使う間は npm の `@ludiars/canalis` (+ playwright と chromium) と tsx を残す必要がある。
  - `/api/spec/extract` の本文上限 (5MB + 1MiB) を超えたときの応答が JSON でなくなる点。
