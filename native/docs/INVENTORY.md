# Discutere C++ 移行 棚卸し (job cpp-node-20261030)

作成 2026-10-29。基点 `main` 5e6b4f5。作業ブランチ `cpp/discutere-node-port`。

目的は脆弱性の受け口を減らすことと、外部依存を増やさないこと (速度は目的にしない)。

## 1. 取得物の版とハッシュ (このセッションで `sha256sum` で確認)

| 物 | 版 | 取得元 | SHA256 | 用途 |
|---|---|---|---|---|
| Boost (Asio/Beast, ヘッダのみ) | 1.87.0 | github boostorg `boost-1.87.0-b2-nodocs.zip` | `40efbb1f38b8cf186fd7bb12809f561ae1c233bae0f75146862e9bfce77d0d42` (指示書の値と一致) | HTTP サーバ/クライアント, WebSocket |
| nlohmann_json | 3.11.3 | github nlohmann `json.tar.xz` | `d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d` (一致) | JSON |
| Lorica | commit `cab4f12086e30484f249d1b4aa479bf7cdec13a9` | github LUDIARS/Lorica | (git commit 固定) | `Lorica::boundary`, `lorica_harden_target` |
| OpenSSL | 3.0.13 (ubuntu `libssl-dev`) | システム | — | TLS, SHA-256, HMAC, Ed25519, 乱数 |
| SQLite | 3.53.4 | npm `better-sqlite3-13.0.3.tgz` 同梱の amalgamation (`deps/sqlite3/sqlite3.c`) | tgz `77e0513dc1a469fb3bceec4c7fb5ad3f403109787eda05be047ec17fd56868cb` / sqlite3.c `6a2805f8c1ef020a7086e62258519323cf98b219e9fe87a42065d916cfebaefc` / sqlite3.h `919e7f2e8ed1d8f56ac17b412b8971c76aa5d1a879752cc6058f75e7d5910e1d` | DB (移行前と同じ実体・同じコンパイル定義) |
| Kuzu | 0.11.3 | npm `kuzu-0.11.3.tgz` | `6cda34f2536c05f831ef86f0e5370b5d0204a51ac3bac99580d9ec5d333c88bf` | **常駐サーバでは未使用** (下記) のため C++ ビルドには入れない |

- sqlite.org への直接取得はこの作業環境のプロキシで 403 になった。better-sqlite3 が実際にコンパイルしている
  amalgamation と同一物を npm tarball (ハッシュ固定) から取り出すので、移行前と完全に同じ SQLite になる。
  コンパイル定義も better-sqlite3 の `deps/download.sh` と揃える。
- Kuzu: `src/core/db/kuzu-client.ts` の `KuzuClient` は名前に反して better-sqlite3 (SQLite) を開く。npm `kuzu` を
  import するのは `src/core/db/kuzu-graph-client.ts` だけで、それを使うのは `scripts/rebuild-kuzu.ts` と
  `scripts/ext-canalis-run.ts` (CLI) のみ。常駐サーバ (`src/index.ts` から到達するコード) は Kuzu を開かない。

## 2. npm 依存の棚卸し (移行前 prod 推移的 159 件, package-lock)

直接依存ごとの推移的件数は package-lock から機械集計 (peer を含むため合計は 172 で、重複を除いた prod 実数は 159)。

| npm 依存 | lock の版 | 推移数 | 常駐サーバでの用途 | 判定 | 理由 |
|---|---|---|---|---|---|
| hono | 4.13.7 | 1 | HTTP ルーティング | 置き換え | `native/src/infra/http_router` (自前の小さなルータ) |
| @hono/node-server | 2.1.1 | 2 | HTTP サーバ | 置き換え | Boost.Beast (serveStatic は使わず, 静的配信は埋め込み/固定パスのみ) |
| discord.js | 14.27.0 | 25 | Gateway / REST | 置き換え | Beast WebSocket + OpenSSL で Gateway v10 / REST v10 を実装 |
| better-sqlite3 | 13.0.3 | 2 | SQLite | 置き換え | 同梱の SQLite amalgamation を直接リンク |
| drizzle-orm | 0.45.2 | 6 | `src/db` のスキーマ/クエリ | 置き換え | 生 SQL (スキーマは同じ DDL) |
| @aws-sdk/client-s3 | 3.1128.0 | 26 | S3 バックアップ | 置き換え | SigV4 を OpenSSL (HMAC/SHA-256) で実装 |
| uuid | 11.1.1 | 1 | ID 生成 | 置き換え | OpenSSL `RAND_bytes` で v4 |
| gray-matter | 4.0.3 | 10 | front matter | 置き換え | YAML の部分集合 (key: scalar / 配列) だけ読む。範囲外は拒否 |
| @ludiars/one-shot | (submodule) | 1 | `claude -p` / `codex exec` 起動 | 置き換え | 引数配列で起動する C++ 実装 (環境変数の遮断・モデル解決も移植) |
| @ludiars/llm-gateway | (submodule) | 1 | tier ルーティング | 置き換え | 純関数 245 行を移植 |
| @ludiars/blackbox | (submodule) | 1 | 情報密度判定のルール卒業 | 置き換え | 純 TS (SQLite store 含む) を移植 |
| @ludiars/sentiment-core | (submodule) | 1 | 語彙ベースの感情スコア | 置き換え | モデル推論は無く語彙辞書と算術のみ (493 行) なので移植 |
| @ludiars/vestigium | (submodule) | 15 | JSONL ログ | 置き換え | JSONL 書き出し・日次切替・保持期限を C++ で実装 (pino/zod 不要) |
| fundamentum | (submodule) | 1 | 議論 export (content-addressed) | 置き換え | SHA-256 と JSON 正規化のみ。OpenSSL で移植 |
| @ludiars/canalis | (submodule) | 5 (playwright 含む) | Notion/公開ページのクロール | **Node のまま** | ブラウザ操作 (playwright) を伴う。サイドカー (別プロセス) で呼ぶ |
| pdf-parse | 2.4.5 | 13 | 仕様書 PDF 抽出 | **Node のまま** | 複雑な外部形式。サイドカー |
| mammoth | 1.12.2 | 26 | 仕様書 DOCX 抽出 | **Node のまま** | 複雑な外部形式。サイドカー |
| tar | 7.5.22 | 6 | バックアップ tar.gz 作成 | **Node のまま** | 指示書により自作しない。サイドカー |
| kuzu | 0.11.3 | 41 | (CLI スクリプトのみ) | 常駐サーバから不要になった | サーバは使わない。CLI は Node のまま |
| ws (推移, 直接 import) | 8.21.3 | — | Slack Socket Mode | 置き換え | Beast WebSocket |
| zod (推移, 直接 import) | 3.25.76 | — | 入力検証 | 置き換え | 手書きの検証関数 |
| electron (dev) | 42.11.3 | (dev 572 件の一部) | デスクトップ殻 | 不要になった | 画面は C++ サーバからブラウザで開く |

## 3. 機能の範囲

- `src/index.ts` から到達する TS: 364 ファイル / 55,520 行 → C++ 移植の対象。
- 到達しない TS (CLI スクリプト専用): 80 ファイル / 6,858 行 (`restructure/*`, `crawler/steam-persona/*`,
  `crawler/comment-history/*`, `flow/proxy/{eval,form,import}`, `core/jobs/*`, `core/bridge/*` 等) → Node のまま (CLI)。
- `scripts/*.ts` (46 本) は運用 CLI。Node のまま。

モジュール別 (到達分, 行数):

| モジュール | 行数 | 移植先 |
|---|---|---|
| flow (直下) | 12,879 | `native/src/flow/` |
| flow/dialectic | 3,850 | `native/src/flow/dialectic/` |
| flow/web | 3,239 | `native/src/flow/web/` (画面 HTML/JS は静的資産として切り出し) |
| flow/proxy, foresight, user-voices, db, anatomia, contracts | 4,001 | `native/src/flow/<sub>/` |
| persona-engine | 5,279 | `native/src/persona_engine/` |
| api | 4,894 | `native/src/api/` |
| discord-hook | 4,531 | `native/src/discord_hook/` |
| core | 2,449 | `native/src/core/` |
| visualize | 2,299 | `native/src/visualize/` |
| crawler (到達分) | 1,912 | `native/src/crawler/` |
| config.ts / startup-guard.ts / index.ts | 2,387 | `native/src/config.*`, `startup_guard.*`, `main.cpp` |
| discatier-engine-adapter | 1,382 | `native/src/discatier_engine_adapter/` |
| slack | 917 | `native/src/slack/` |
| その他 (db, discussion, persona-bridge, kg-sync, ludus, discussion-seed, level-check, cernere-service-token, backup, runtime-settings, analysis, design-diagnostic, queue, integrations, fundamentum-export, external-discussion, observability, middleware, feedback, mechanics-check, web-chat, secrets) | 約 5,600 | 同名ディレクトリ (`-` は `_`) |

TS → C++ の名前の規則: `src/a/b-c.ts` → `native/src/a/b_c.{hpp,cpp}`、名前空間 `di::a` (入れ子のディレクトリは入れ子の名前空間)。
関数は snake_case、型は PascalCase のまま、JSON に出る struct のフィールドは TS のプロパティ名のまま。

## 4. 並行性の設計

Node の単一スレッド + Promise を、C++ では次の形にする。

- HTTP サーバ: Beast の非同期受付 (I/O スレッド 2 本) → ハンドラは上限付きの worker pool で実行。
- 外部 HTTP / `claude -p`: ハンドラ側から同期 API で呼ぶ (内部は呼び出しごとの io_context と期限)。
- Discord Gateway / Slack Socket Mode: 専用スレッドの WebSocket。イベントは worker pool に渡す。
- 議論の進行 (flow): worker pool のジョブとして同期的に進める。同時実行数に上限を持つ。
- SQLite: 接続ごとに mutex で直列化 (`SQLITE_THREADSAFE=2`)。

## 5. 安全の対応表 (移植前の防御 → C++ で同じ挙動)

| 防御 | 移植前 | C++ |
|---|---|---|
| HTTP bind 既定 loopback | `config.server.host` 既定 `127.0.0.1` | 同じ既定 (`BACKEND_HOST`) |
| `/api/*` userContext | `X-User-Id`/`X-User-Role` ヘッダ、無ければ loopback=admin、他は anonymous | 同じ (`middleware/auth`) |
| admin role 必須の各 API | 401 (userId 無し) / 403 (role≠admin) | 同じ |
| `/internal/worker/*` loopback 限定 | 非 loopback は 403 | 同じ |
| external-discussion 共有秘密 + body 160KB 上限 | timingSafeEqual / bodyLimit | `CRYPTO_memcmp` + ルート別本文上限 |
| conclusion publication | 32 文字未満は 503, timingSafeEqual, ローカル操作は loopback + Host + Origin 検査 | 同じ |
| glab review trends トークン | timingSafeEqual | 同じ |
| persona-bridge | PASETO v4.public (Ed25519) 検証 / 固定トークン 32 文字以上 / 503 default-deny | OpenSSL Ed25519 で同じ |
| kg-migration トークン | tokenOk → 403 | 同じ |
| Discord 認証境界 | bot token の Gateway 接続 + admin-id allowlist、未設定なら admin コマンド全拒否 | 同じ (未設定=全拒否) |
| bot token をログに出さない | — | ログ書き出しで token を出さない。Authorization ヘッダはログしない |
| Di 管理フォーラム以外を無視 | guild / 親 forum ID 照合 (`managed-forum-scope`) | 同じ |
| 外部の声の個人アンカーを仮名化 | 出力 serializer で `論者#xxxx` | 同じ |
| `機密`/`内部` タグで外部の声を参照しない | `tags.gateExternalVoices` | 同じ |
| Voluptas URL は HTTPS (loopback のみ HTTP 可) | URL 検査 | 同じ |
| CORS | `frontendUrl` のみ許可, credentials | 同じ |
| startup guard | production で botToken 等必須 / dev default 秘密を拒否 | 同じ |
| 子プロセス | `@ludiars/one-shot` (shell:false, API キー系 env を遮断) | `posix_spawn` / `CreateProcessW` に引数配列。env 遮断も同じ |
| ZIP 展開上限 (PR #3487) | spec-source の上限 | 仕様書抽出は Node サイドカーのまま (同じ上限) |

C++ で新しく付けるもの: 全外部入力 (HTTP 本文・WS フレーム・外部 HTTP 応答・子プロセス出力・JSON) への
`lorica::input_budget` 課金、全 target への `lorica_harden_target()`、外部 I/O の期限と同時実行上限、
利用者が指定する URL への SSRF 対策 (解決した IP を検査して固定する)。

## 6. Node のまま残すもの (サイドカー / CLI)

| 機能 | 理由 | 呼び方 |
|---|---|---|
| 仕様書の抽出 (PDF/DOCX/XLSX/PPTX/ZIP) `flow/spec-source.ts` | 複雑な外部形式 (指示書 5) | `native/sidecar/di-sidecar.ts` を引数配列で起動 (JSON in/out, 期限・出力上限) |
| Canalis クロール (Notion/公開ページ, playwright) | ブラウザ操作 (指示書 5) | 同上 |
| バックアップの tar.gz 作成 | 指示書 5 (`tar`) | 同上。S3 送信 (SigV4) は C++ |
| `scripts/*.ts` と CLI 専用モジュール (80 ファイル) | 常駐サーバではない | 変更なし |
