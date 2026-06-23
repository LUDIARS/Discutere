# Discatier KG ストア二層化 — index=SQLite / KG本体=Kuzu

> 対象: `src/core/` のストレージ層。`spec/data/core/DESIGN.md`（イベントソーシング+グラフDB）の
> 「グラフDB」を、プレースホルダの SQLite から **本物の Kuzu** へ昇格させる移行設計。
> 関連: 取込基盤 [Canalis](https://github.com/LUDIARS/Canalis)（`KgBatch`→`KuzuSink` で接合）/
> `spec/feature/crawler/SENTIMENT.md`（感情は 0+1 カスケード）/ `spec/feature/backup/DESIGN.md`。

## 1. 背景と決定

- 現状の `KuzuClient`（`src/core/db/kuzu-client.ts`）は **名前だけ Kuzu で中身 better-sqlite3**。
  `kuzu` npm は未導入。一方 `spec/data/core/DESIGN.md` は明確に「イベントソーシング + **グラフ DB**」と
  謳い、横断クエリ（`docs/codex-tasks/phase-6-cross-queries.md` の `/lineage` `/find`）は
  **既に Cypher 前提**。＝ グラフDB化は設計の既定路線で、SQLite は暫定実装だった。
- **決定（2026-06-09）**: ストアを二層に分ける。
  - **KG 本体（ノード + 関係）= Kuzu（本物のグラフDB）**
  - **index / 運用データ = SQLite（現状維持）**
- **`events`（追記専用イベントログ）が唯一の真実の源 (SoT)**。Kuzu は events から投影される
  **replay 可能な派生ビュー**（いつでも破棄して再構築できる）。これにより移行は低リスク。
- `kuzu` の Node binding は **Windows 動作実績あり**（運用者確認済、2026-06-09）。前提リスクはクリア。

## 2. ストア責務分割

| データ | 行き先 | 理由 |
|---|---|---|
| `events` | **SQLite** | 唯一の真実の源。append-only。Kuzu はここから派生 |
| `people` / `games` / `mechanics` / `aesthetics` / `affects` / `play_contexts` / `sessions` / `utterances` / `reactions` / `design_gaps` / `hypotheses` | **Kuzu (ノード)** | KG 本体。多ホップ探索（/lineage 等）の対象 |
| 上記間の関係（現 FK カラム） | **Kuzu (REL)** | グラフ traversal の実体 |
| `embeddings`（vector_json） | **SQLite** | ベクトル索引。Kuzu のネイティブ vector は未成熟。`/find` のベクトル検索はここ |
| `translation_proposals` | **SQLite** | レビュー待ち queue（運用ワークフロー、グラフではない） |
| `hypothesis_validations` | **SQLite** | 検証ログ（append 的） |
| caches（hotspot-rank / uncategorized-clusters）/ `_migrations` | **SQLite** | 派生キャッシュ・移行管理 |
| 全文索引（将来 FTS5） | **SQLite** | `/find` の全文側 |

> 原則: **グラフとして辿るもの＝Kuzu / ログ・queue・索引・ベクトル＝SQLite**。

## 3. Kuzu グラフスキーマ (DDL)

ノードは `id` を PRIMARY KEY、`workspace_id` を絞り込み用プロパティとして保持（マルチサーバは
KG 単一集約・session は guild 別、の現方針を踏襲）。時刻は epoch ms (INT64)。

```cypher
-- ノード（FK カラムは REL へ移すため node プロパティから除外）
CREATE NODE TABLE Person     (id STRING, workspace_id STRING, name STRING, role STRING,
                              created_at INT64, updated_at INT64, PRIMARY KEY(id));
CREATE NODE TABLE Game       (id STRING, workspace_id STRING, title STRING, genre STRING,
                              created_at INT64, updated_at INT64, PRIMARY KEY(id));
CREATE NODE TABLE Mechanic   (id STRING, workspace_id STRING, name STRING, description STRING,
                              intends STRING, intended_affect STRING,
                              created_at INT64, updated_at INT64, PRIMARY KEY(id));
CREATE NODE TABLE Aesthetic  (id STRING, workspace_id STRING, name STRING, description STRING,
                              created_at INT64, updated_at INT64, PRIMARY KEY(id));
CREATE NODE TABLE Affect     (id STRING, workspace_id STRING, mood STRING, score DOUBLE,
                              valence STRING, vocabulary_status STRING,
                              created_at INT64, updated_at INT64, PRIMARY KEY(id));
CREATE NODE TABLE PlayContext(id STRING, workspace_id STRING, platform STRING, mode STRING,
                              created_at INT64, updated_at INT64, PRIMARY KEY(id));
CREATE NODE TABLE Session    (id STRING, workspace_id STRING, title STRING, started_at INT64,
                              ended_at INT64, mode STRING, scene STRING,
                              created_at INT64, updated_at INT64, PRIMARY KEY(id));
CREATE NODE TABLE Utterance  (id STRING, workspace_id STRING, raw_content STRING,
                              normalized_content STRING, posted_at INT64,
                              created_at INT64, updated_at INT64, PRIMARY KEY(id));
CREATE NODE TABLE Reaction   (id STRING, workspace_id STRING, reaction_type STRING,
                              intensity DOUBLE, created_at INT64, updated_at INT64, PRIMARY KEY(id));
CREATE NODE TABLE DesignGap  (id STRING, workspace_id STRING, title STRING, description STRING,
                              status STRING, gap_in STRING, expected_affect STRING,
                              observed_affect STRING, evidence_json STRING,
                              created_at INT64, updated_at INT64, PRIMARY KEY(id));
CREATE NODE TABLE Hypothesis (id STRING, workspace_id STRING, statement STRING, status STRING,
                              integrated BOOLEAN, validated_by_emotion BOOLEAN,
                              stale_flagged BOOLEAN, refs_json STRING,
                              created_at INT64, updated_at INT64, PRIMARY KEY(id));

-- 関係（現 FK + phase-6 Cypher 由来）。Kuzu は 1 REL 表に複数 FROM-TO 対を持てる
CREATE REL TABLE SPOKEN_BY     (FROM Utterance TO Person);          -- utterances.speaker_id
CREATE REL TABLE IN_SESSION    (FROM Utterance TO Session,          -- utterances.session_id
                                FROM Affect    TO Session);          -- affects.session_id
CREATE REL TABLE RESPONDS_TO   (FROM Utterance TO Utterance);       -- utterances.responds_to
CREATE REL TABLE REACTS_ON     (FROM Reaction TO Utterance);        -- reactions.utterance_id
CREATE REL TABLE REACTED_BY    (FROM Reaction TO Person);           -- reactions.actor_id
CREATE REL TABLE OF_GAME       (FROM Mechanic    TO Game,           -- mechanics.game_id
                                FROM Aesthetic   TO Game,           -- aesthetics.game_id
                                FROM PlayContext TO Game);          -- play_contexts.game_id
CREATE REL TABLE ABOUT         (FROM Affect TO Mechanic,            -- affects.subject_id (多態)
                                FROM Affect TO Aesthetic);
CREATE REL TABLE IN_GAME       (FROM DesignGap TO Game);            -- design_gaps.game_id
CREATE REL TABLE ADDRESSES     (FROM Hypothesis TO DesignGap);      -- hypotheses.design_gap_id
CREATE REL TABLE REFINES       (FROM Utterance TO Mechanic);        -- phase-6: 発話→機構の精緻化
CREATE REL TABLE INTEGRATES_AS (FROM Hypothesis TO Mechanic);      -- phase-6: 仮説→機構の統合
```

## 4. 投影のファンアウト（移行の肝）

書き込みは全て events 経由（CQRS）なので、ストアを引き剥がさず **`applyEventProjection`
（`src/core/events/projection.ts`）を 2 系統にファンアウト**するだけ：

```
DomainEvent (events = SoT / SQLite, atomic append)
        │  applyEventProjection
        ├──▶ SQLite: index/運用テーブル (embeddings/translation_proposals/… を維持)
        └──▶ Kuzu:   ノード MERGE + 関係 MATCH+MERGE   ← 追加
```

イベント → Kuzu 反映の対応（主系列）:

| イベント | Kuzu 操作 |
|---|---|
| `UtteranceCreated` | `MERGE (:Utterance)` + `SPOKEN_BY`/`IN_SESSION`、`responds_to` あれば `RESPONDS_TO` |
| `UtteranceNormalized` | `SET u.normalized_content` |
| `ReactionAdded` | `MERGE (:Reaction)` + `REACTS_ON` + `REACTED_BY` |
| `MechanicProposed` | `MERGE (:Mechanic)` + `OF_GAME` |
| `AffectMeasured` | `MERGE (:Affect)` + `IN_SESSION` + `ABOUT(subject)` |
| `DesignGapDetected` / `DesignGapUpdated` | `MERGE (:DesignGap)` + `IN_GAME`、`SET status…` |
| `HypothesisProposed` … `HypothesisIntegrated` | `MERGE (:Hypothesis)` + `ADDRESSES`、integrate 時 `INTEGRATES_AS` |

- 反映は **冪等 MERGE**（Canalis `KuzuSink` と同方式: `MERGE (n:Label {id:$id}) SET n += $props`）。
- **結果整合でよい**: events を atomic 書込 → Kuzu 投影。Kuzu 投影が失敗しても events から
  **replay で再構築**できる（§6）。

## 5. Canalis との接合

外部クロール（YouTube/Reddit）経由の取込は **Canalis** に載せる:

```
Canalis ① youtube/reddit adapter → RawRecord[]
Canalis ② Di:comment-sentiment (SENTIMENT.md の 0+1 カスケード) → KgBatch
          (nodes: Utterance/Reaction/Affect, edges: SPOKEN_BY/IN_SESSION/REACTS_ON/ABOUT…)
Canalis ③ KuzuSink (MERGE) → 本 Kuzu
```

- Di② が出す `KgBatch` のノード/エッジ label は §3 の Kuzu スキーマに一致させる。
- ただし **SoT は events**。Canalis 経由でも「イベント発行 → 投影」を通すのが正（Canalis 直
  Kuzu 書きで events を迂回しない）。具体的には Di② は `AffectMeasured` 等のイベントを発行し、
  §4 のファンアウトが Kuzu に反映する。Canalis `KuzuSink` を使うのは events 迂回が許される
  バルク再構築/バックフィル時に限定する（要・運用線引き）。
- index（embeddings 等）は Di 内部の責務で **Canalis スコープ外**。

## 6. 整合性・再構築・障害時

- **再構築コマンド**（新規）: Kuzu を drop → events を時系列 replay → ノード/関係を再投影。
  起動時に「Kuzu スキーマ版数 < 現行」なら自動再構築も可。
- **検証**: replay 後、SQLite read model（現行テーブル）件数と Kuzu ノード件数を type 別に照合。
  移行期は両投影を並走させ差分ゼロを確認してから SQLite KG read テーブルを撤去。
- **バックアップ**（`spec/feature/backup/DESIGN.md`）: tar 対象に **Kuzu DB ディレクトリ**を追加
  （Kuzu はディレクトリ単位の DB）。SQLite と合わせて 1 アーカイブ。

## 7. クラス/配線の変更

- `KuzuClient`（実は SQLite）→ **`SqliteIndexClient` にリネーム**（誤解の元を除去）。
- 新規 **`KuzuGraphClient`**（`src/core/db/kuzu-graph-client.ts`）: `kuzu` npm をラップ、
  embedded DB、§3 DDL を冪等適用、Cypher 実行 + パラメタ。
- `createCore()`（`src/core/index.ts`）の配線: `client`(SQLite index) と `graph`(Kuzu) を併設。
  repositories は引き続き **イベント発行のみ**（直書きしない）。投影が両ストアへ振り分ける。
- リソース寿命（規約§4）: 両 client の `close()` を全経路で確実に。

## 8. 移行手順（段階・各段 PR）

1. **kuzu 導入** — `kuzu` npm 追加。`KuzuGraphClient` + §3 DDL の冪等適用。最小 smoke
   （Win で起動・MERGE・MATCH が通る）。*Windows 実績は確認済なので gate ではなく確認。*
2. **投影ファンアウト** — `applyEventProjection` に Kuzu 反映を追加（dual-write）。SQLite 側は当面維持。
3. **再構築コマンド + 検証** — replay で Kuzu 再構築、SQLite read model と件数照合 (差分0)。
4. **横断クエリを Cypher へ** — `/lineage` `/find`(グラフ側) 等を Kuzu Cypher 実装へ切替
   （phase-6 の Cypher 例をそのまま）。ベクトル/FTS は SQLite 併用のまま。
5. **SQLite KG read テーブル撤去** — events からの KG 投影を Kuzu 一本化。
   `embeddings`/`translation_proposals`/`hypothesis_validations`/caches/`events` は残す。
6. **backup 更新** — Kuzu dir を tar 対象に追加。

## 9. リスク / 未決

- **Kuzu の vector / 全文**: 当面 SQLite(embeddings + FTS5) 側に置く前提。Kuzu の vector
  extension 成熟を見て将来再評価。
- **同時実行**: Kuzu embedded の書込は単一プロセス想定。events SoT が単一直列なので投影も
  直列化する（並行投影しない）。マルチ guild でも KG は単一集約の現方針と整合。
- **`feat/di-datasource-transparency` との順序**: 出所メタ（source+URL）が Utterance に付く設計
  なので、そのマージ後に Utterance ノードへ provenance プロパティを足す（本 spec は構造のみ規定、
  プロパティ追加はマージ内容に合わせる）。
- **大規模 replay 性能**: events 蓄積が増えた場合の再構築時間。バッチ MERGE / トランザクション
  まとめで対処（実装時に計測）。

## 10. 受け入れ条件

- [ ] `KuzuGraphClient` が Windows で DDL 適用 + MERGE/MATCH 実行できる
- [ ] events replay で Kuzu を再構築し、SQLite read model とノード件数が type 別に一致
- [ ] `/lineage` が任意 Mechanic を起点に Utterance/Session/Hypothesis を Cypher で 3 軸横断
- [ ] dual-write 期に SQLite/Kuzu の差分ゼロを確認できる検証コマンドがある
- [ ] backup が Kuzu DB ディレクトリを含む
- [ ] `embeddings` / `translation_proposals` 等の index は SQLite に残り従来どおり動く
