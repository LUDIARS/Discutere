# Phase 1: 保存層コア API

設計参照: `docs/discatier_implementation_plan.md` §2 (データモデル) / §2.4 (Event Sourcing) / §5 (データ保存スタック)

## ゴール

Kuzu グラフを保存層として、 ノード・エッジ・イベントの CRUD と Event Sourcing の projection 基盤を提供する。 後続 Phase はすべてこの API の上に乗る。

## 前提

- Phase 0 が完了し、 以下が固定済:
  - Kuzu スキーマ DDL
  - イベント型と payload の TypeScript 型定義
  - 初期 Affect 語彙 (20-40件) 投入済
  - 埋め込みモデルとベクトル次元

## ディレクトリ構成 (新規)

```
src/core/
  db/
    kuzu-client.ts           # Kuzu 接続シングルトン
    schema.ts                # スキーマ DDL を文字列で保持 + migration 実行
    types.ts                 # ノード/エッジ/イベントの TypeScript 型
  events/
    event-types.ts           # EventType + payload union
    event-log.ts             # Event 永続化 (Kuzu + JSONL)
    projection.ts            # event → グラフ反映
  repositories/
    person.ts
    game.ts
    mechanic.ts
    aesthetic.ts
    affect.ts
    play-context.ts
    session.ts
    utterance.ts
    reaction.ts
    design-gap.ts
    hypothesis.ts
  vectors/
    embedding.ts             # 埋め込み生成ラッパー
    vector-search.ts         # Kuzu ARRAY 列でのコサイン類似検索
  index.ts                   # public API のバレル
tests/core/
  ...                        # 各リポ + projection の単体テスト
  e2e-session-utterance.test.ts  # 受け入れ条件用
```

## サブタスク

1. **Kuzu クライアント初期化**
   - 接続シングルトン、 close hook、 migration 自動実行。
   - DB ファイルパスを env `DISCATIER_KUZU_PATH` から取得 (default: `./data/discatier.kuzu`)。

2. **スキーマ migration**
   - §2.1 のノード型 9 種 + Event テーブル + §2.2 のエッジ型を Kuzu DDL で発行。
   - `Utterance.raw_content` は UPDATE 拒否 (Kuzu トリガー or アプリ層で immutable check)。
   - 適用済 migration を `_migrations` テーブルで管理。

3. **TypeScript 型定義**
   - 全ノード型 / エッジ型 / EventType (§2.4 の 15 種) を export。
   - payload は型付き union (`MechanicProposedPayload`, `UtteranceCreatedPayload` 等)。

4. **イベントログ層**
   - `appendEvent(event)`: Kuzu `Event` テーブル + JSONL ファイル (`./data/events.jsonl`) に append。
   - `loadEventsSince(eventId | timestamp)`: 範囲取得。
   - 排他制御 (single-writer 前提でも明示)。

5. **Projection**
   - 起動時に `events.jsonl` (または Kuzu Event) を読み込み、 メモリ上でグラフ状態を再構築。
   - 受信した `appendEvent` ごとに increment 反映。
   - 不整合検出時は full rebuild API を提供。

6. **CRUD リポジトリ**
   - 各ノード型に対し `create / get / update (不変なものは禁止) / list` を提供。
   - 書き込みは必ず Event 経由 (`appendEvent` 内部呼び出し)。 直接 INSERT は禁止。

7. **ベクトル API**
   - `registerEmbedding(nodeId, vector)`: Kuzu ARRAY 列に格納。
   - `searchSimilar(vector, k, filter?)`: コサイン類似度 top-k。

## 受け入れ条件

- [ ] スキーマ migration が空 DB で成功する。 再実行は no-op。
- [ ] `Person` を 1 件作成し、 `events.jsonl` に `PersonCreated` 相当が記録される (または対応 Event 型)。
- [ ] `Session` 起動 → `Utterance` 1 件作成 → DB を閉じて再起動 → projection で完全に同じグラフが再構築される。
- [ ] `Utterance.raw_content` を UPDATE しようとすると拒否される。
- [ ] 全リポジトリの単体テストが通る (各リポ最低 3 ケース: create / get / list)。
- [ ] ベクトル登録 + 類似検索の単体テストが通る (mock 埋め込みで OK)。

## スコープ外

- メッセージ受信 → Event 発行のルーティング (Phase 2)
- LLM 呼び出し (Phase 3 以降)
- スラッシュコマンド (Phase 2 以降)

## コミット & PR

- ブランチ: `feat/discatier-phase-1-storage-core`
- PR 単位: 1 PR で完結 (サブタスク横断)。 ファイルが多くなる場合のみ 2 分割可、 ただし依存関係に注意。
- マージ後、 `codex-tasks/README.md` 進捗表に merge hash を記録。
