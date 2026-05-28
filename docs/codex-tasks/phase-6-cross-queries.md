# Phase 6: 横断クエリコマンド

設計参照: `docs/discatier_implementation_plan.md` §3.4 (横断コマンド) / §7.4 (語彙の早期固定化対策)

## ゴール

3軸どこからでも呼べる横断コマンド `/lineage` `/cluster` `/stale` `/hotspot` `/find` `/me role` を実装する。 結果はチャットに返せる構造化テキストで返却。 加えて、 未語彙化クラスタリングとホットスポット集計のバッチジョブを用意する。

## 前提

- Phase 1-5 完了 (merge hash 確認)。
- Phase 2 で stub 化した `cross/` ハンドラを置き換える形で実装。

## ディレクトリ構成

```
src/core/projection/command-handlers/cross/
  me-role.ts                 # /me role <theorist|player|developer>
  find.ts                    # /find <query> (全文 + ベクトル)
  lineage.ts                 # /lineage <mechanic_id>
  cluster.ts                 # /cluster (未語彙化クラスタ最新)
  stale.ts                   # /stale (30日停滞 Hypothesis)
  hotspot.ts                 # /hotspot (活発度・Gap 集中度)
  index.ts                   # ルータ (stub を置換)

src/core/jobs/
  uncategorized-clustering-job.ts  # /cluster の元データ生成
  hotspot-aggregation-job.ts       # /hotspot の元データ生成

src/core/cache/
  uncategorized-clusters.ts        # クラスタ結果キャッシュ
  hotspot-rank.ts                  # ホットスポットランクキャッシュ

tests/core/projection/cross/
  me-role.test.ts
  find.test.ts
  lineage.test.ts
  cluster.test.ts
  stale.test.ts
  hotspot.test.ts
```

## サブタスク

1. **`/me role`**
   - 現在 Person の roles を更新し、 現セッションを `SessionEnded` で閉じて新規 Session を起動。
   - mode は新 role に応じて自動推定するか、 引数で明示要求。

2. **`/find`**
   - 入力クエリを埋め込みベクトル化し、 ベクトル検索 + Kuzu の全文インデックス両方で候補取得。
   - 結果は Mechanic / Affect / Hypothesis / Utterance を score 順に最大 20 件、 種別ラベル付きで返却。

3. **`/lineage`**
   - 指定 Mechanic の `refines` チェーン + それを生んだ Session / Utterance / Hypothesis を 3 軸横断で辿る。
   - Cypher 例:
     ```cypher
     MATCH (m:Mechanic {id: $mid})
     OPTIONAL MATCH (u:Utterance)-[:refines]->(m)
     OPTIONAL MATCH (u)-[:in_session]->(s:Session)
     OPTIONAL MATCH (h:Hypothesis)-[:integrates_as]->(m)
     RETURN m, collect({u: u, s: s}) AS history, collect(h) AS hypotheses
     ```
   - 時系列で構造化テキストに整形。

4. **`/cluster`**
   - `uncategorized-clustering-job.ts` の最新出力を返す。
   - ジョブ自体は cron 1 日 1 回 + Translation Bridge の reject/「新語彙必要」 イベント時にトリガー。
   - 処理: `Affect` にマップされない / `uncategorized` フラグの Utterance を埋め込みでクラスタリング (HDBSCAN 系 or 簡易 k-means)。
   - 各クラスタの代表発話 + サイズ + 代表埋め込み近傍 Affect を返す。

5. **`/stale`**
   - Phase 5 で付けた `stale_flagged=true` の Hypothesis を一覧。
   - 「いつから停滞」「現状態」「提案 Session」 付きで構造化テキストに整形。

6. **`/hotspot`**
   - `hotspot-aggregation-job.ts` の最新出力を返す。
   - 集計指標 (Mechanic 単位):
     - 直近 N 日の Utterance 言及回数
     - 紐づく未解決 DesignGap 数
     - 紐づく非終端 Hypothesis 数
   - 重みは const にして調整可能。 cron 1 日 1 回。

7. **共通**
   - 構造化テキスト返却は Markdown ベース (表 + リスト)。
   - 軸ごとに権限フィルタを通すこと (例: Axis 2 から `/cluster` 結果を出す場合は Affect 名を伏せて感情のサンプルだけ提示する等、 §3.2 の鉄則に従う)。

## 受け入れ条件

- [ ] `/lineage` が任意 Mechanic の現在定義からそれを生み出した発話・議論・検証を 3 軸横断で辿る (E2E)。
- [ ] `/cluster` が未語彙化感情クラスタを返す (シードデータで E2E、 クラスタ数 ≥ 1)。
- [ ] `/stale` が 30日停滞 Hypothesis を返す (Phase 5 の stale_flagged 連動)。
- [ ] `/hotspot` が Mechanic を活発度順に返す (シードデータで E2E)。
- [ ] `/find` がクエリに対し関連度の高い候補を返す (mock 埋め込みで可)。
- [ ] `/me role` で Session が切り替わる (E2E)。
- [ ] Axis 2 から `/cluster` を呼ぶと理論用語が出ない (§3.2 の鉄則テスト)。

## スコープ外

- 個別 UI レンダリング
- LLM による補足説明 (構造化テキストだけ返す)

## コミット & PR

- ブランチ: `feat/discatier-phase-6-cross-queries`
- base: Phase 5 の merge hash
- PR 単位: 1 PR。 コマンド + ジョブ + キャッシュをまとめて出す。
