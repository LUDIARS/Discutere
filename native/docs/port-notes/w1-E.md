# port-notes w1-E: flow の基礎部品・感情ライブラリ・Cernere トークン・仕様書の取り込み

job `cpp-node-20261030`。担当の TS 31 ファイル + `@ludiars/sentiment-core` を C++ に移した。

## 1. 機能の対応表 (TS → C++)

| TS | C++ |
|---|---|
| `lib/lapilli/packages/sentiment-core/src/{lexicon,vector,affect-score,cascade,design-gap,speaker-profile,vocabulary,index}.ts` + `data/{lexicon,vocabulary}.json` | `native/src/lib/sentiment_core/{lexicon,lexicon_data,vector,affect_score,cascade,design_gap,speaker_profile,vocabulary,js_number}.{hpp,cpp}`, `sentiment_core.hpp` (辞書は `lexicon_data.cpp` に原文のまま埋め込み) |
| `src/crawler/sentiment/cascade.ts` | `native/src/crawler/sentiment/cascade.hpp` (`di::crawler::sentiment`、再エクスポート) |
| `src/analysis/affect-score.ts` | `native/src/analysis/affect_score.hpp` (`di::analysis`、再エクスポート) |
| `src/flow/sentiment-vector.ts` | `native/src/flow/sentiment_vector.hpp` (`di::flow`、再エクスポート) |
| `src/flow/argument-graph.ts` | `native/src/flow/argument_graph.{hpp,cpp}` |
| `src/flow/contracts/runtime.ts` | `native/src/flow/contracts/runtime.{hpp,cpp}` (`run_contract<R>` が `contract(fn, spec)` の同期版) |
| `src/flow/contracts/spec-zip-entry.contract.ts` | `native/src/flow/contracts/spec_zip_entry_contract.{hpp,cpp}` (ZIP 展開はサイドカーのため、判定関数として保持) |
| `src/flow/cost-report.ts` | `native/src/flow/cost_report.{hpp,cpp}` |
| `src/flow/cost-relay.ts` | `native/src/flow/cost_relay.{hpp,cpp}` (`start_cost_relay` は `global_scheduler` + `background_executor`、`CostRelayTicker` が 1 tick) |
| `src/flow/dialectic/convergence.ts` | `native/src/flow/dialectic/convergence.{hpp,cpp}` |
| `src/flow/dialectic/premise-rules.ts` | `native/src/flow/dialectic/premise_rules.{hpp,cpp}` |
| `src/flow/discussion-redo.ts` | `native/src/flow/discussion_redo.{hpp,cpp}` |
| `src/flow/github-cli.ts` | `native/src/flow/github_cli.{hpp,cpp}` (`di::proc::run` に引数配列) |
| `src/flow/investigate.ts` | `native/src/flow/investigate.{hpp,cpp}` (gray-matter → `di::lib::front_matter::parse`) |
| `src/flow/paper-blocks.ts` | `native/src/flow/paper_blocks.{hpp,cpp}` |
| `src/flow/paper-review-intent.ts` | `native/src/flow/paper_review_intent.{hpp,cpp}` + `unicode_letter_number.inc` (生成物) |
| `src/flow/paper-review-session.ts` | `native/src/flow/paper_review_session.{hpp,cpp}` (`run_serial` は届いた順の順番札) |
| `src/flow/persona-display.ts` | `native/src/flow/persona_display.{hpp,cpp}` |
| `src/flow/persona-history-evidence.ts` | `native/src/flow/persona_history_evidence.{hpp,cpp}` (zod strict → 手書きの検査) |
| `src/flow/persona-import-document.ts` | `native/src/flow/persona_import_document.{hpp,cpp}` |
| `src/flow/personas.ts` | `native/src/flow/personas.{hpp,cpp}` (`FlowPersona` の `to_json` / `flow_persona_from_json` も) |
| `src/flow/proxy/attribution.ts` | `native/src/flow/proxy/attribution.{hpp,cpp}` |
| `src/flow/proxy/contract.ts` | `native/src/flow/proxy/contract.{hpp,cpp}` |
| `src/flow/proxy/prompt.ts` | `native/src/flow/proxy/prompt.{hpp,cpp}` |
| `src/flow/sparring-commands.ts` | `native/src/flow/sparring_commands.{hpp,cpp}` |
| `src/flow/spec-source.ts` | `native/src/flow/spec_source.{hpp,cpp}` + `native/sidecar/spec-extract.ts` (PDF/DOCX/PPTX/XLSX/旧 DOC) |
| `src/flow/tags.ts` | `native/src/flow/tags.{hpp,cpp}` |
| `src/flow/turn-scheduler.ts` | `native/src/flow/turn_scheduler.{hpp,cpp}` |
| `src/flow/voluptas-persona-client.ts` | `native/src/flow/voluptas_persona_client.{hpp,cpp}` |
| `src/cernere-service-token/paseto-v4-public.ts` | `native/src/cernere_service_token/paseto_v4_public.{hpp,cpp}` |
| `src/cernere-service-token/cernere-public-keys.ts` | `native/src/cernere_service_token/cernere_public_keys.{hpp,cpp}` |
| `src/cernere-service-token/service-token-client.ts` | `native/src/cernere_service_token/service_token_client.{hpp,cpp}` |
| (補助) | `native/src/flow/text_scan.{hpp,cpp}` (引用の走査・空白除去・E 番号。正規表現の代わり) |

golden 生成スクリプト: `native/tests/golden/w1-E-{sentiment_core,flow_pure,cost,investigate,spec_source,unicode_ln}.gen.ts`
(`npx tsx native/tests/golden/<名前>.gen.ts`)。`w1-E-unicode_ln.gen.ts` は `/[\p{L}\p{N}]/u` の範囲表 `native/src/flow/unicode_letter_number.inc` を Node から生成する。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| `lib/lapilli/packages/sentiment-core/tests/vector.test.ts`, `tests/sentiment-core/golden.test.ts` (50 文ダイジェスト・集計ベクトル) | `lib.sentiment_core` |
| `tests/flow/argument-graph.test.ts` | `flow.argument_graph` |
| `tests/flow/paper-blocks.test.ts` | `flow.paper_blocks` |
| `tests/flow/paper-review-intent.test.ts` | `flow.paper_review_intent` |
| `tests/flow/paper-review-session.test.ts` | `flow.paper_review_session` |
| `tests/flow/turn-scheduler.test.ts` (輪番部分), `tests/flow/model-roster.test.ts` (roster 割当), `tests/flow/persona-display.test.ts`, tags | `flow.personas` |
| `tests/flow/discussion-redo.test.ts`, `tests/flow/sparring.test.ts` (コマンド検出), `tests/flow/conclusion-convergence.test.ts` (収束判定の文言), `tests/flow/rule-check.test.ts` (前提ルールの正規化部分), `tests/flow/persona-bridge.test.ts` (persona 文書の読み) | `flow.small_pure` |
| `tests/flow/proxy-debate.test.ts` (契約・帰属・prompt) | `flow.proxy` |
| `tests/flow/cost-relay.test.ts` | `flow.cost_relay` |
| `tests/flow/discussion.test.ts` (investigateTheme / loadMechanics) | `flow.investigate` |
| (runtime.ts のテンプレート挙動) | `flow.contracts_runtime` |
| `tests/flow/cernere-service-token.test.ts` (PASETO・公開鍵 loader・発行 client・Voluptas pull) | `cernere_service_token.cernere_service_token` |
| (github-cli はテスト無し) | `flow.github_cli` (偽の gh スクリプトで) |
| `tests/flow/spec-source.test.ts`, `src/flow/spec-source.test.ts` | `flow.spec_source` (ZIP の上限そのものはサイドカー側の同じ TS コードが担うので、TS の `src/flow/spec-source.test.ts` が引き続き有効) |
| `tests/crawler/comment-history.test.ts` (履歴根拠の期限・purge) | `flow.persona_history_evidence` |

ctest 全体: 23/23 通過 (土台 + w1-E。ビルドディレクトリ `/home/user/build-w1-E`)。

## 3. 安全の対応表

| 防御 / 入力経路 | C++ |
|---|---|
| `機密`/`内部` タグで外部の声を参照しない (`gateExternalVoices`)・外部収集しない (`canCollectExternal`) | 同じ (`flow::gate_external_voices` / `can_collect_external`、investigate の YouTube 補完も同じ条件) |
| PASETO v4.public 署名検証 (PAE・base64url の文字検査・footer を署名に含める・どれか 1 鍵で通れば可) | OpenSSL Ed25519 (`crypto::ed25519_verify`)。token 16KiB 上限、payload は `parse_json` (16KiB/1 万要素/深さ 16) |
| Cernere 公開鍵: 10 分キャッシュ・失敗後 30 秒の backoff・redirect を追わない | 同じ。応答 256KiB 上限、期限 10 秒、mutex で守る |
| service token 発行: credentials は HTTPS (loopback のみ HTTP) の Cernere にだけ・redirect を追わない・token はメモリだけ | 同じ (`max_redirects=0`)。応答 64KiB 上限、期限 15 秒、token をログに出さない |
| Voluptas URL は HTTPS (loopback のみ HTTP)・Authorization を redirect 先へ送らない・100 ページ上限・固定トークンをログに出さない | 同じ。1 ページ 16MiB 上限、NDJSON は行ごとに `parse_json` (16MiB/200 万要素/深さ 64) |
| proxy パック: URL・個人特定フィールドの拒否、仮名 speakerKey、根拠 id の検査 | 同じ (エラー文言と順序まで golden で一致) |
| 帰属検査 (束に無い id・原文に無い引用を本人発言にしない) | 同じ |
| 仕様書: Discord はローカルパスを読ませない (`allowLocalPath=false`)、拡張子の許可リスト、5MB 上限 | 同じ。ファイルは `fs::read_file(max)` で上限を超えて確保しない。NUL を含むパスは不在扱い |
| 仕様書 URL の取得 | **新たに SSRF 対策** (`allow_private_network=false`)。応答は `max_response_bytes = maxBytes` |
| 仕様書 HTML の除去 (二乗時間だった) | `di::str::strip_html` (線形)。2MiB の `<` 連続・20 万個の `<script>` でも 5 秒以内を確認 |
| PDF/DOCX/ZIP の抽出・ZIP 展開上限 (PR #3487) | サイドカーで同じ TS コード。C++ からは入力 64MiB 上限・期限 120 秒・出力 32MiB。サイドカーはパスを受け取らずバイト列だけ、方式名は許可リスト、stdin 上限、依存の console 出力は stderr へ |
| gh の起動 | 引数配列 (シェル不使用)、期限 5 秒、出力 1MiB、トークンを伏せる (`redact_secrets`) |
| cost-feed への送信 | 期限 15 秒・応答 1MiB。送信先は設定済みのローカルサービス (`allow_private_network=true`) |
| DB の値 | `sql::QueryLimits` で課金 (集計は 100 万行/256MiB) |
| LLM 応答 (感情 cascade) | `{...}` 部分を `parse_json` (64KiB/1 万/深さ 32) |
| data/games の md・*.sentiment.json | 4MiB / 64MiB 上限で読み、`parse_json` / front matter の上限 |
| 正規表現 | すべて手書きの線形走査 (`std::regex` 不使用)。巨大入力のテストあり |

## 4. 移していないもの・挙動の差

- **PDF・DOCX・PPTX・XLSX・旧 DOC の本文抽出**は Node のまま (`native/sidecar/spec-extract.ts`)。複雑な外部形式のパーサーと zlib を自作しないため。
  - サイドカーには方式名 (`spec.pdf` 等) を渡すので、URL 経路で「Content-Type は docx/pptx だが中身が PDF のバイト列」のときだけ移植前と抽出方式が変わりうる (移植前は Content-Type の方式、今はバイト列が PDF なら PDF)。
- 仕様書 URL の取得は SSRF 対策で非公開アドレス (loopback・社内 IP) に繋がらなくなった (移植前は繋がった)。上限超過時の文言は応答の大きさが分からないので `(> N bytes)`。
- `strip_html` の実体参照: `&#x110000;` 等の範囲外の数値参照で、移植前は例外 (RangeError) だったが今は U+FFFD。
- 不正な UTF-8 は WHATWG の TextDecoder と同じ置換単位で復号する (`decode_utf8_like_textdecoder`)。
- `toLowerCase` は `di::str::to_lower` (ASCII・Latin-1・全角英字)。それ以外の文字種の大文字小文字は変えない (感情辞書・検索語では影響なし)。
- `loadThemeMechanics`: 読めない front matter のファイルは移植前は例外で全体が失敗したが、今はそのファイルを飛ばして warn する (front matter の対応範囲が gray-matter より狭いため、全体を止めない側に倒した)。ファイルの列挙順は名前順 (Node の readdir も libuv が名前順に並べるので同じ)。
- `parsePersonaDocument`: 文書全体が上限 (16MiB/200 万要素/深さ 64) を超えると NDJSON として読み直す (移植前は上限なし)。
- `parseHistoryEvidence`: zod の詳細なエラーではなく `std::invalid_argument` (キー名と理由) を投げる。期限・前後関係の文言は同じ。
- `run_serial`: TS の Promise 連鎖と同じく同じスレッド id は届いた順に 1 つずつ (順番札)。
- `start_cost_relay`: 前の tick が走っている間の次の tick は飛ばす (移植前は重なって走りえた)。
- `shuffle` / `pick_random_persona`: rng が [0,1) を外れた値を返しても範囲外の要素を引かない (移植前は undefined)。
- `cascade` の LLM 応答で polarity が配列 (`["positive"]`) のような異形は拒否する (移植前は文字列化して通していた)。
- `github_cli`: Node の execFile のエラー文言は「Command failed: gh …」「spawn gh ENOENT」を真似ているが完全一致ではない (available の判定に使う ENOENT / not found は同じ)。
- `GithubSpecSource` の JSON のキー順は owner, repo, path, ref に固定 (移植前は URL の形でまちまち)。値は同じ。
- `fetch_github_repo_info` / `fetch_github_spec_text` の通信は利用者指定ではない api.github.com だが SSRF 検査は既定 (公開アドレスのみ) のまま。
- `service-token-verify.ts` (exp/aud/scope の検査) は担当外 (後の wave)。PASETO の層は署名と形式だけを見る (移植前と同じ責務分担)。テストでは期限切れ・aud 違いのトークンが署名としては通り、claims がそのまま返ることを確かめた。

## 5. 土台に欲しいもの / 人間の判断が要ること

- **土台に欲しいもの**
  - `js_to_fixed` (Number#toFixed と同じ丸め) と `js_to_number` (JSON 値の Number()) を `base/json` に。今は `lib/sentiment_core/js_number.hpp` に置いて flow からも使っている。
  - `str::is_letter_or_number` (`\p{L}\p{N}`) を `base/strings` に。今は `flow/unicode_letter_number.inc` (Node から生成) を `flow/paper_review_intent.cpp` が持つ。
  - WHATWG 互換の UTF-8 復号 (`decode_utf8_like_textdecoder`) を `base/strings` に。`str::sanitize_utf8` は不正バイトごとに U+FFFD を出すので TextDecoder と数が違う。
  - `fs::list_dir` の並びが Node (libuv) と同じ strcmp 順であることの明記。
  - `http::fetch` の応答上限超過を区別できるエラー種別 (今は文言に "too large"/"limit" を含むかで判定している)。
  - `env::set_for_test` が `std::getenv` の値 (const char*) をそのまま受けられるとテストが書きやすい (今は nullptr を渡さないよう注意が要る)。
- **人間の判断が要ること**
  - 仕様書 URL の SSRF 対策で、Web (loopback 信頼) から社内・ローカルの URL の仕様書を読む運用があれば、Web 経路だけ `allow_private_network=true` にするか決める。
  - サイドカー `spec-extract.ts` を使う間は npm の `pdf-parse` / `mammoth` と tsx を残す必要がある。
  - `loadThemeMechanics` で読めない front matter を「飛ばす」挙動でよいか (移植前は例外)。
