# 移植の規約 (TS → C++)

job `cpp-node-20261030`。依存の方針と目的は `native/docs/INVENTORY.md`。ここは書き方の規約。

## 置き場所と名前

- `src/a/b-c.ts` → `native/src/a/b_c.hpp` + `native/src/a/b_c.cpp`。ディレクトリ名の `-` は `_`。
- 名前空間は `di::<ディレクトリ>` (入れ子は入れ子: `src/flow/dialectic/x.ts` → `di::flow::dialectic`)。
  `src/config.ts` は `di::config`、`src/index.ts` は `native/src/main.cpp`。
- 関数は snake_case (`buildPaperDraft` → `build_paper_draft`)。型は PascalCase のまま。定数は元の名前のまま。
- JSON に出る (HTTP 応答・DB の JSON 列・LLM への入出力) struct のフィールド名は **TS のプロパティ名のまま** (camelCase)。
  `to_json(const T&) -> di::Json` / `T from_json(const di::Json&)` を同じヘッダに置く。キーの順序は TS のオブジェクトリテラルの順。
- TS の `@ludiars/*` / `fundamentum` パッケージは `native/src/lib/<パッケージ名>/`。

## 土台 (native/src/base, native/src/infra) — 編集しない

各モジュールの移植では base / infra / CMake を編集しない (並行作業の衝突を避けるため)。足りない汎用部品は
自分のモジュール内に置き、`native/docs/port-notes/<担当>.md` に「土台に欲しいもの」として書く。

| TS / Node | C++ |
|---|---|
| `JSON.parse` (外部入力) | `di::parse_json(text, limits)` (lorica で大きさ・要素数・深さを課金) |
| `JSON.stringify` | `di::json_stringify` / `json_stringify_pretty` (JS と同じ書式) |
| `Json` 型 | `di::Json` = `nlohmann::ordered_json` (挿入順を保つ) |
| 文字列の `.length` / `.slice` | `di::str::js_length` / `js_slice` (UTF-16 単位)。`trim` / `squish` / `to_lower` 等も `base/strings.hpp` |
| 正規表現 | **std::regex は使わない** (遅い・再帰で落ちる)。線形の走査を手で書く |
| `Date.now()` / `toISOString()` | `di::time::now_ms()` / `di::time::iso_utc()` |
| `Math.random()` / `randomUUID()` | `di::crypto::random_unit()` / `di::crypto::uuid_v4()` |
| `createHash` / `createHmac` / `timingSafeEqual` | `di::crypto::sha256` / `hmac_sha256` / `constant_time_equal` |
| `console.log/warn/error` | `di::log::info/warn/error` (秘密値を渡さない) |
| `process.env.X` | `di::env::get("X")` / `get_nonempty` |
| `fs.readFileSync` 等 | `di::fs::read_file(path, max_bytes)` 等 (`base/fsutil.hpp`)。利用者が指定するパスは `fs::safe_join` |
| better-sqlite3 | `di::sql::Database` (`run` / `get` / `all` / `exec` / `transaction`)。DB の値も上限つき |
| `fetch` | `di::http::fetch(ClientRequest)`。期限・応答上限を必ず指定。ローカルの設定済みサービスへは `allow_private_network = true`、利用者が指定する URL は false のまま (SSRF 対策) |
| `spawn` / `execFile` | `di::proc::run(Options)` (引数配列のみ)。`claude -p` / `codex exec` は `di::lib::one_shot::run` |
| `LLMClient` | `di::persona_engine::LLMClient` (`persona_engine/llm/client.hpp`)。`invoke` は同期 |
| Hono のルート | `di::http::Router` (`get/post/use/mount`)。本文上限は `RouteOptions{max_body}` |
| `Promise.all` | `di::parallel_map(items, fn, max_parallel)` (上限必須) |
| 投げっぱなしの Promise | `di::background_executor().submit(fn)` (false = キュー満杯 → 拒否か縮退を決めて書く) |
| `setInterval` / `setTimeout` | `di::global_scheduler().every/after` (重い仕事は executor に渡す) |
| 画面 (HTML/CSS/ブラウザ JS) | 移植しない。`native/web/` にファイルで置き `di::web::find(name)` で配信 (`native/web/README.md`) |

## 非同期 → 同期

- `async function f(): Promise<T>` は `T f()` にする。待ち (LLM・HTTP・子プロセス) は呼んだスレッドで待つ。
- 議論の進行は worker のスレッド上で同期的に進める。HTTP ハンドラから長い仕事を始めるときは
  `background_executor().submit` に渡して即座に応答する (TS が `void run()` していたところ)。
- モジュール内の可変な状態 (TS の module-level `let` や `Map`) は mutex で守る。

## 外部入力 (安全の規則)

- HTTP 本文・WebSocket・外部 HTTP 応答・子プロセス出力・ファイル・DB の値は、確保・展開の前に上限を課す
  (`parse_json` の limits、`fetch` の `max_response_bytes`、`proc::Options::max_stdout`、`fs::read_file` の max_bytes、
  独自の配列・再帰には `lorica::input_budget` / `lorica::checked_*` を直接使う)。
- 既存の防御 (認証・認可・トークン比較・loopback 限定・本文上限・URL 検査・仮名化・機密タグの外部の声遮断) は
  同じ挙動で移す。弱めない。移したものは port-notes の安全の対応表に書く。
- 秘密値をソース・テスト・ログに書かない。テストの秘密値は明らかに偽物の値にする。

## テスト

- 元の TS テスト (`tests/**`, `src/**/*.test.ts`) を同じ観点で `native/tests/<モジュール>/<名前>_tests.cpp` に移す
  (`tests/check.hpp` の `TEST_CASE` / `CHECK` / `CHECK_EQ`)。1 ファイル = 1 実行ファイル = 1 ctest。
- 互換性 (golden): Node 実装の入出力を `native/tests/golden/<モジュール>.gen.ts` (tsx で実行) で
  `native/tests/fixtures/<モジュール>/*.json` に書き出し、C++ のテストが同じ出力を返すことを確かめる。
  生成スクリプトはコミットする (`npx tsx native/tests/golden/<モジュール>.gen.ts`)。
- 境界: 各入力経路に「上限ちょうど・上限+1・壊れた入力・巨大な入力」を入れる。
- 本物の外部サービス (Discord・Slack・S3・LLM・Cernere・Voluptas) には繋がない。偽のサーバ (テスト内で
  `http::Server` を立てる) か記録した応答を使う。
- ビルドは `native/tools/build.sh <自分のビルドディレクトリ>` (同時フルビルド 2 本まで、-j2)。

## port-notes

各担当は `native/docs/port-notes/<担当>.md` に次を書く (最終報告の材料):
1. 機能の対応表 (TS ファイル → C++ ファイル)
2. テストの対応表 (TS テスト → ctest 名)
3. 安全の対応表 (移した防御・lorica の課金を入れた入力経路)
4. 移していないもの・挙動の差 (理由つき)
5. 土台に欲しいもの / 人間の判断が要ること
