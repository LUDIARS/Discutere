# port-notes w1-C — データ層 (core・db・flow/db・kg-sync) と front matter の書き出し

job `cpp-node-20261030`。golden 生成: `npx tsx native/tests/golden/w1-C-data.gen.ts` /
`npx tsx native/tests/golden/w1-C-front_matter_dump.gen.ts`。

## 1. 機能の対応表 (TS → C++)

| TS | C++ |
|---|---|
| src/core/cache/hotspot-rank.ts | native/src/core/cache/hotspot_rank.{hpp,cpp} (`di::core::cache`) |
| src/core/cache/uncategorized-clusters.ts | native/src/core/cache/uncategorized_clusters.{hpp,cpp} |
| src/core/db/kuzu-client.ts | native/src/core/db/kuzu_client.{hpp,cpp} (`KuzuClient` = SQLite。WAL / foreign_keys / migrations も同じ) |
| src/core/db/schema.ts | native/src/core/db/schema.{hpp,cpp} (`core_migrations` / `apply_migrations`) |
| src/core/events/event-types.ts | native/src/core/events/event_types.{hpp,cpp} (`EVENT_TYPES`、payload struct + `to_json`、`DomainEvent`) |
| src/core/events/projection.ts | native/src/core/events/projection.{hpp,cpp} |
| src/core/hypothesis/state-machine.ts | native/src/core/hypothesis/state_machine.{hpp,cpp} |
| src/core/noise/exclusions.ts | native/src/core/noise/exclusions.{hpp,cpp} |
| src/core/projection/command-parser.ts | native/src/core/projection/command_parser.{hpp,cpp} |
| src/core/projection/responds-to-inference.ts | native/src/core/projection/responds_to_inference.{hpp,cpp} |
| src/core/vectors/embed-retry.ts | native/src/core/vectors/embed_retry.{hpp,cpp} |
| src/core/vectors/embedder.ts | native/src/core/vectors/embedder.{hpp,cpp} |
| src/core/vectors/hybrid-rank.ts | native/src/core/vectors/hybrid_rank.{hpp,cpp} |
| src/core/vectors/vector-math.ts | native/src/core/vectors/vector_math.{hpp,cpp} |
| src/db/connection.ts | native/src/db/connection.{hpp,cpp} (`get_main_db`) |
| src/db/schema.ts (drizzle) | native/src/db/schema.{hpp,cpp} (drizzle-kit と同じ DDL・行 struct・`TABLE` 定数・`ensure_schema`) |
| src/discatier-engine-adapter/game-aliases.ts | native/src/discatier_engine_adapter/game_aliases.{hpp,cpp} |
| src/discatier-engine-adapter/keyword-terms.ts | native/src/discatier_engine_adapter/keyword_terms.{hpp,cpp} |
| src/discatier-engine-adapter/ludus-term-aliases.ts | native/src/discatier_engine_adapter/ludus_term_aliases.{hpp,cpp} (中身は golden で TS と一致を確認) |
| src/flow/db/connection.ts | native/src/flow/db/connection.{hpp,cpp} (`get_flow_db`) |
| src/flow/db/migrations.ts | native/src/flow/db/migrations.{hpp,cpp} |
| src/kg-sync/export.ts | native/src/kg_sync/export.{hpp,cpp} |
| src/kg-sync/import.ts | native/src/kg_sync/import.{hpp,cpp} |
| src/kg-sync/shared-tables.ts | native/src/kg_sync/shared_tables.{hpp,cpp} |
| src/kg-sync/state.ts | native/src/kg_sync/state.{hpp,cpp} |
| gray-matter `matter.stringify` / js-yaml 3.15.2 `safeDump` | native/src/lib/front_matter/front_matter_dump.cpp (`stringify` / `dump_yaml`) |
| (局所的な補助) | native/src/core/db/js_bind.hpp — better-sqlite3 と同じ JS 値 → SQLite の bind 規則 |

core の部品は `createCore` (src/core/index.ts, 後の段階) がそのまま組める形: `KuzuClient` が
`sql::Database` を持ち、`apply_event_projection(db, event)`・`noise::*`・`vectors::*` は `sql::Database&` を取る。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| tests/core/hybrid-rank.test.ts | core.hybrid_rank (+ golden) |
| tests/core/embed-retry.test.ts、tests/core/query-embed-cache.test.ts の設定検証部分 | core.embedder (+ 偽の OpenAI 互換サーバ) |
| tests/core/projection/command-parser.test.ts、tests/core/hypothesis/state-machine.test.ts | core.pure (+ golden、caches、noise exclusions) |
| tests/core/run.ts の migration 再実行 | core.schema (sqlite_master golden、既存 DB への追加適用) |
| (無し) projection | core.projection (golden: Node で射影した後の全行) |
| tests/flow/foundation.test.ts の「DB マイグレーション」3 件 | flow_db.migrations (+ sqlite_master golden、duplicate column の無視、get_flow_db) |
| (無し) drizzle スキーマ | db.schema (drizzle-kit の DDL と sqlite_master が golden と一致) |
| tests/persona-engine/game-aliases.test.ts | discatier_engine_adapter.game_aliases (+ golden、Ludus 辞書の中身) |
| tests/persona-engine/external-voices-lookup.test.ts の extractKeyTerms 節 | discatier_engine_adapter.keyword_terms (+ golden) |
| tests/kg-sync/run.ts | kg_sync.kg_sync (+ golden の NDJSON・import 結果・行、上限、状態ファイル) |
| (無し) gray-matter stringify | lib.front_matter_dump (golden 190 件、1 バイト単位で一致) |

## 3. 安全の対応表

| 入力経路 | 防御 |
|---|---|
| 埋め込みサーバの応答 (embedder) | `max_response_bytes` 64MiB、`parse_json` (要素 2,000 万・深さ 16)、期限 = 設定 timeoutMs (接続は 10 秒まで)、リダイレクトを追わない (fetch の `redirect: "error"` と同じ)、HTTP 本文をエラー文言・ログに入れない、同時実行はプロセス全体の fetch 上限 32 |
| 埋め込みの設定 URL | http/https 以外・認証情報入り・クエリ/フラグメント付きを拒否 (TS と同じ文言)。設定済みのローカルサービスなので `allow_private_network=true` |
| リトライ | 試行回数・待ち時間の検査 (無限リトライを防ぐ)。1e9 回を超える指定は 1e9 に収める |
| kg-sync import (pull した応答・受け取った本文) | 全体 256MiB・行数 200 万を書き込み前に `lorica::input_budget` で課金 (超えたら何も書かずに `std::length_error`)。1 行 4MiB・要素 10 万・深さ 32 (超えた行は壊れた行と同じく ignored)。テーブル whitelist (任意行注入の防止) と列は手元の PRAGMA の列だけ (識別子は手元のスキーマ由来)。1 transaction で全か無か |
| kg-sync export (DB の値) | テーブルごとに行数 100 万・512MiB (`sql::QueryLimits`) |
| kg-sync 状態ファイル | 16MiB まで、壊れた/大きすぎるファイルは空の状態。書き込みは一時ファイル + rename |
| `table_columns` | 識別子を "..." で囲む (TS は文字列をそのまま PRAGMA に埋め込んでいた) |
| front matter dump | 入れ子 256 段で拒否 (再帰の上限)。長い文字列も線形 (正規表現を使わない) |
| keyword_terms / game_aliases / responds-to / command-parser | std::regex を使わず線形の走査 |

## 4. 移していないもの・挙動の差

- **Kuzu グラフ (src/core/db/kuzu-graph-client.ts, src/core/events/kuzu-projection.ts) は移さない**。常駐サーバは
  Kuzu グラフを開かない (createCore に graph を渡す経路が無い。npm kuzu を使うのは scripts/rebuild-kuzu.ts 等の
  CLI だけ)。C++ の core は graph 無しの挙動 (TS で graph を渡さないときと同じ) だけを持つ。CLI は Node のまま。
- **drizzle**: クエリ API は移さない (後続の担当が生 SQL で書く)。行 struct・`TABLE` 定数・列の型の読み方
  (boolean=0/1、timestamp=Unix 秒、timestamp_ms=Unix ミリ秒) を `db/schema.hpp` に置いた。
  TS は実行時にテーブルを作らず `drizzle-kit push` に任せていた。C++ では drizzle-kit が無いので
  `get_main_db()` が **無いテーブルだけを drizzle-kit と同じ DDL で作る** (既存のテーブル・列には触らない)。
- kg-sync の `tableColumns` は TS では DB ごとに永続キャッシュしていた。C++ は 1 回の export/import の中だけで
  使い回す (スキーマ変更後も古い列一覧を使わない。結果は同じ)。
- kg-sync import の上限超過 (全体・行数) は TS には無い新しい拒否。1 行の上限超過は ignored に数える。
- 埋め込みのエラー文言の理由句 (`res.statusText`) は C++ の HTTP クライアントが返さないので、標準の理由句表で補う
  (ログ表示だけの差)。応答上限を超えた応答は通信失敗として扱うので一過性に分類される (最大 attempts 回)。
- front matter: `stringify` に BMP 外の文字を含む「文字列」を data として渡した場合 (JS では UTF-16 の片割れの
  キーになる) は `std::invalid_argument`。JSON の object/array を渡す通常の使い方では差は無い。
  JS で同じ object を 2 回参照したときの `&ref_` / `*ref_` は C++ の値渡しの Json では起きない。
  読めない front matter を含む content は gray-matter と同じく例外 (`std::runtime_error`)。
- TS テスト tests/core/hybrid-rank.test.ts の「両系上位の b が a より高い」は Node 実装でも同点 (1/61+1/62) で
  落ちる。C++ のテストは Node 実装の値 (golden) に合わせて同点を確かめる。
- `get_flow_db` / `get_main_db` は TS の module-level インスタンスと同じく 1 つを共有する (mutex で保護)。

## 5. 土台に欲しいもの・人間の判断が要ること

- `http::ClientResponse` に理由句 (reason phrase) があると `res.statusText` を正確に写せる。
- `http::ClientResponse` で「応答上限超過」とそれ以外の通信失敗を区別できると、埋め込みのリトライ判定を
  恒常エラー扱いにできる (今は文字列の error しか無い)。
- 人間の判断: `get_main_db()` が無いテーブルを自動で作る (drizzle-kit push の代わり) ことの可否。
- 人間の判断: kg-sync import で `null` の行があると TS は TypeError で取り込み全体が失敗する。C++ も同じにしたが、
  ignored に数える方が妥当かもしれない。
