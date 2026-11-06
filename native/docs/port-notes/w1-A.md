# port-notes: w1-A (設定・バックアップ・ブリッジ等)

job `cpp-node-20261030`。担当 14 ファイル + lib `fundamentum`。

## 1. 機能の対応表 (TS → C++)

| TS | C++ | 名前空間 / 主な関数 |
|---|---|---|
| `src/config.ts` | `native/src/config.{hpp,cpp}` | `di::config` — `DiscutereConfig` (フィールドは TS のまま camelCase、`kgSync.auto` だけ `auto_`)、`load_config(LoadOptions{env, cwd})`、`get_config()` (初回に読んでキャッシュ)、`reset_config_for_test()`、`to_json()` |
| `src/flow/vote-lenses.ts` | `native/src/flow/vote_lenses.{hpp,cpp}` | `di::flow::VoteLens`, `DEFAULT_VOTE_LENSES()` |
| `src/backup/archive.ts` | `native/src/backup/archive.{hpp,cpp}` + `native/sidecar/backup-archive.ts` | `di::backup::create_archive` → `di::sidecar::run("backup-archive.ts")` (tar.gz 作成は Node) |
| `src/backup/s3.ts` | `native/src/backup/s3.{hpp,cpp}` | `di::backup::upload_file_to_s3`、SigV4 (`sigv4_*`)、`resolve_credentials` |
| `src/discussion-seed/genres.ts` | `native/src/discussion_seed/genres.{hpp,cpp}` | `GENRE_SEEDS()`, `build_genre_topic`, `pick_genre_seed` |
| `src/discussion-seed/store-trends.ts` | `native/src/discussion_seed/store_trends.{hpp,cpp}` | `fetch_store_trend_topics(limit, StoreTrendSource)` |
| `src/external-discussion/contracts.ts` | `native/src/external_discussion/contracts.{hpp,cpp}` | `parse_conversation`, `locale_compare` |
| `src/external-discussion/participation.ts` | `native/src/external_discussion/participation.{hpp,cpp}` | `opportunity`, `SILENCE_MS` |
| `src/external-discussion/store.ts` | `native/src/external_discussion/store.{hpp,cpp}` | `ParticipationStore::{claim,finish,fail}` (BEGIN IMMEDIATE) |
| `src/feedback/store.ts` | `native/src/feedback/store.{hpp,cpp}` | `GameFeedbackStore` |
| `src/integrations/conclusion-publication.ts` | `native/src/integrations/conclusion_publication.{hpp,cpp}` | `publication_state`, `set_publication`, `published_conclusions` |
| `src/persona-bridge/authorization-assertion.ts` | `native/src/persona_bridge/authorization_assertion.{hpp,cpp}` | `verify_persona_bridge_assertion` (+ 例外 2 種) |
| `src/persona-bridge/utterance-export.ts` | `native/src/persona_bridge/utterance_export.{hpp,cpp}` | カーソル encode/decode/next、`read_human_utterances` |
| `src/queue/snapshot.ts` | `native/src/queue/snapshot.{hpp,cpp}` | `build_queue_snapshot`, `format_queue_text` |
| `lib/fundamentum/src/**` | `native/src/lib/fundamentum/fundamentum.{hpp,cpp}` | `di::lib::fundamentum` — `canonicalize`, `content_id_of`, `MemoryBackend` / `FileBackend`, `MasterStore`, `CatalogStore`/`CatalogBuilder`, `UserStore` (`delete` は `remove`), `Resolver`, `Foundation::{in_memory,on_disk}` (同期 API) |

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト / golden | ctest |
|---|---|
| `tests/flow/vote-lenses.test.ts` (config 部分), `tests/flow/foundation.test.ts` (config), `tests/core/query-embed-cache.test.ts` (batchSize 検査), golden `w1-A-config.gen.ts` (13 パターン: 既定・env 上書き・config ファイル・env 優先・壊れた/配列/null ファイル・正整数エラー 4 種・既定パス) | `config.config` |
| `lib/fundamentum/src/__tests__/{hash,master,file-backend,user,resolver,integration}.test.ts`, golden `w1-A-fundamentum.gen.ts` (canonical 23 件 + FileBackend のファイル配置) | `lib.fundamentum` |
| `tests/flow/external-discussion.test.ts` (parse / opportunity / store), golden `w1-A-external_discussion.gen.ts` | `external_discussion.external_discussion` |
| `tests/feedback/store.test.ts` | `feedback.store` |
| `tests/visualize/conclusion-publication.test.ts` (データ層), golden `w1-A-integrations.gen.ts` | `integrations.conclusion_publication` |
| `tests/flow/persona-bridge.test.ts` (readHumanUtterances・カーソル・アサーション拒否), golden `w1-A-persona_bridge.gen.ts` (26 アサーション + 16 カーソル) | `persona_bridge.persona_bridge` |
| golden `w1-A-queue.gen.ts` (TS テスト無し) | `queue.snapshot` |
| golden `w1-A-discussion_seed.gen.ts` (偽の Steam 応答、TS テスト無し) | `discussion_seed.seeds` |
| (TS テスト無し) AWS SigV4 テストベクタ + 偽 S3 | `backup.s3` |
| (TS テスト無し) サイドカーで実際に tar.gz を作る | `backup.archive` |

ルート (`external-discussion-routes` / `conclusion-publication-routes` / `persona-bridge` routes) と `ExternalDiscussionService` は担当外 (別担当が移す)。

## 3. 安全の対応表

| 入力経路 / 防御 | C++ |
|---|---|
| config ファイル (`DISCUTERE_CONFIG`) | `fs::read_file` 4 MiB 上限 + `parse_json` (要素 20 万・深さ 64)。超過・壊れた入力は TS と同じく警告して無視 (既定値) |
| env の投票レンズ JSON | `parse_json` 64 KiB / 1 万要素 / 深さ 16。失敗は既定レンズ |
| 正の整数必須 (embedding.timeoutMs / batchSize) | TS と同じく fail-fast (`std::runtime_error` 同文言) |
| persona bridge アサーション | 順序・文言を TS と同一: 形式 → 大きさ (署名計算の前に payload 2731 文字 / 署名 86 文字) → base64url → Ed25519 検証 (`di::crypto::ed25519_verify`) → payload 2048 バイト・JSON (lorica 課金) → authorId snowflake・一致 → aud → exp 整数 → jti → 有効期限 5 分以内。公開鍵未設定・不正は設定エラー (呼び出し側で 503 / default-deny) |
| persona bridge カーソル | 512 文字・`[A-Za-z0-9_-]`・JSON 上限・safe integer・id 256 (UTF-16) |
| 外部会話の入力 (`parseConversation`) | scene 正規表現 (線形走査)・80 件・id 200 / text 8000 (UTF-16)・合計 32000・safe integer の at・重複 id 拒否 |
| 外部会話の保存結果 (DB 値) | `parse_json` 64 KiB 上限 |
| 結論の公開 | 内容ハッシュ (sha256) 一致でのみ公開/変更、空の結論は公開不可、境界を越えるのは 5 項目のみ、最大 200 件 |
| Steam featuredcategories | 期限 8 秒、応答 4 MiB 上限 (`max_response_bytes` + `parse_json`)、`allow_private_network=false` (SSRF)。失敗は静的フォールバック |
| S3 | 期限 (要求ごと 10 分・接続 15 秒)、応答 1 MiB 上限、同時 upload 1 本 (空き待ち 10 分で拒否)、リダイレクトを追わない、AWS 既定 endpoint は SSRF 検査あり (運用者が設定した endpoint のみ private 可)。例外文言に秘密値を含めない |
| 共有認証ファイル (~/.aws/credentials) | 256 KiB 上限 |
| fundamentum の保存値 | 読み戻しに 64 MiB / 400 万要素 / 深さ 128 の上限。FileBackend は hash 部が `[0-9A-Za-z_-]` 以外の id を読まない・書かない (TS は `fm1:../..` のような id でパスを脱出できた) |
| tar.gz 作成サイドカー | 引数配列で起動 (シェル不使用)、期限 30 分、出力 1 MiB、入力 JSON 1 MiB・targets 1000 件 |
| SQLite (`queue` / `feedback` / `external_discussion` / `integrations` / `persona_bridge`) | `sql::Database` (行・バイト上限つき)。SQL は TS と同一 |

## 4. 移していないもの・挙動の差

- **tar.gz 作成**は Node のサイドカー (`native/sidecar/backup-archive.ts` が `src/backup/archive.ts` をそのまま呼ぶ)。指示どおり tar を自作しない。`tar` npm 依存はこの機能のために残る。
- **S3 の認証**: AWS SDK の既定チェーンのうち env と共有認証ファイルだけ実装。IAM ロール (EC2 IMDS / ECS / SSO / web identity) は未対応 (下記 5)。
- **S3 の送り方**: 64 MiB 超は multipart upload (SDK は単一 PutObject を stream で送っていた)。オブジェクトの key / storage class / content-type は同じだが ETag の形式 (`-N` 付き) が変わる。
- **config の素通し配列**: TS は `workerPool.workers` / `discussion.opinionModels` / `discatier.knowledgeGraphs` / `flow.voteLenses` の要素を config ファイルの値のまま (余分なキー・型違いも) 通していた。C++ は型付きの struct に読むので、余分なキーは落ち、型違いの値は既定値 ("" / 0) になる (knowledgeGraphs の label は文字列のときだけ)。正しい形の設定では同じ (golden で確認)。
- **数値**: TS の `number` は `double` で持つ (config ファイルの値をそのまま写すため)。
- **locale_compare**: 外部会話の同時刻メッセージの並べ替え (`localeCompare`) は ICU を持たないため、ASCII は ICU root と同じ順 (node で表を確認)、非 ASCII は ASCII の後に code point 順で近似。
- **fundamentum**: entries は `di::Json` (挿入順)。JS の「整数風のキーを先に並べる」オブジェクト順と `k in obj` のプロトタイプ参照 (`"toString"` 等の名前) は再現しない。`history` は壊れた (循環する) parent で止まる (TS は無限ループ)。
- **公開鍵の SPKI DER**: Ed25519 の固定 12 バイト前置き + 32 バイトの厳密な形だけ受ける (Node の `createPublicKey` は末尾の余分なバイト等を受ける可能性がある)。設定エラーの扱いは同じ。
- **config の JSON 数値のあふれ** (`1e400`): JS は Infinity → 既定値、nlohmann は parse エラー → ファイル全体を無視、の差がありうる (未検証の境界)。

## 5. 土台に欲しいもの / 人間の判断が要ること

- **人間の判断**: S3 の認証を IAM ロール (IMDS v2 / ECS) まで対応するか。現在の運用 (Ex Vault が env に AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY を注入) なら不要。必要なら IMDS (リンクローカルのメタデータアドレス) への接続を `allow_private_network` で許す設計判断が要る。
- **人間の判断**: S3 multipart の閾値 (64 MiB)。単一 PUT の 5 GiB 上限まで 1 回で送りたい場合はメモリ使用量 (本文の 2 倍) との兼ね合い。
- **土台に欲しいもの**: `http::fetch` の本文をファイル/ストリームから送る口 (S3 の単一 PUT で大きなファイルをメモリに載せずに済む)。
- **土台に欲しいもの**: Node 互換の寛容な base64url decode (`Buffer.from(s, "base64url")`)。`crypto::base64_decode` は長さ 4n+1 を拒否するため、fundamentum と persona_bridge にそれぞれ局所の実装を置いた。
- **土台に欲しいもの**: `sql::Database::transaction` の IMMEDIATE 版 (`external_discussion/store.cpp` で局所に書いた)。
- **土台に欲しいもの**: `String#localeCompare` の共通実装 (今は `external_discussion::locale_compare` に近似を置いた)。
