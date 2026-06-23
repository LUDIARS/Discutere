# Discatier Core — 議論エンジンの中核

> 実装: `src/core/` (`createCore(path?, eventsPath?)` → `src/core/index.ts`)

「遊びの議論プラットフォーム (Discatier)」の心臓部。Discord 等の入口
(`discord-hook` / `persona-engine` / `facilitator`) から独立した、
**イベントソーシング + グラフ DB による議論知識基盤**。発話を取り込み、
設計上の論点 (designGap) を見つけ、仮説 (hypothesis) を立て、検証・統合する。

## レイヤ構成

```
createCore()
  ├ client: KuzuClient          ← グラフ DB (db/kuzu-client.ts, db/schema.ts)
  ├ eventLog: EventLog          ← 追記専用イベントログ (events/event-log.ts)
  ├ repos:  …Repo × 11          ← ノード CRUD (repositories/, ctx 経由でイベント発行)
  └ vectors: registerEmbedding / searchSimilar  ← 類似検索 (vectors/)
```

書き込みは **すべてイベント経由**。repo の更新が `EventLog` に `DomainEvent` を
追記し、`applyEventProjection` (`events/projection.ts`) が SQLite の読み取り用テーブル
(people / sessions / utterances / design_gaps / hypotheses …) へ射影する
(CQRS 風の read model)。

## ドメインノード (repositories/)

11 リポジトリ = グラフのノード型:

| ノード | 役割 |
|---|---|
| `person` | 参加者 (匿名 workspace、個人特定情報は保存しない) |
| `session` | 議論セッション (`title='discussion-of-gap:<gapId>'`、`scene` で Discord 紐付け) |
| `utterance` | 発話 (raw / normalized、`respondsTo` で返信関係) |
| `reaction` | リアクション (👀 等、意見スコアの素) |
| `game` / `mechanic` / `aesthetic` / `affect` / `playContext` | 遊びの知識グラフ (KG) の構成要素 |
| `designGap` | 設計上の論点・課題 (議論の素)。status: open → closed/converged |
| `hypothesis` | 仮説 (gap に対する解) |

## イベント型 (events/event-types.ts)

`EVENT_TYPES` に集約。主系列:

- **発話系**: `UtteranceCreated` / `UtteranceNormalized` / `ReactionAdded`
- **論点系**: `DesignGapDetected` / `DesignGapUpdated`
- **仮説系**: `HypothesisProposed` / `HypothesisDebated` / `HypothesisValidated` /
  `HypothesisIntegrated` / `HypothesisRejected` / `HypothesisStaleFlagged`
- **KG 系**: `MechanicProposed` / `TranslationProposed` / `AffectMeasured` …

## 仮説ライフサイクル (hypothesis/)

`state-machine.ts` が遷移を厳格に検証する有限状態機械:

```
draft ──submit──▶ proposed ──validate_theory──▶ under_theory_validation
                     │                                  │ session_end
                     │ validate_emotion (※直行も許容)    ▼
                     └──────────────▶ under_emotion_validation ◀── validated_by_theory
                                            │ session_end
                                            ▼
                                    validated_by_emotion ──integrate──▶ integrated (終端)
  (どの状態からでも) ──reject──▶ rejected (終端)
```

- **theory → emotion の厳密順は強制しない**: 議論モードでは `proposed` から直接
  emotion 検証へ進めることを許容する (厳密化が要るケースは consumer 側で wrap)。
- `lifecycle-handler.ts` が遷移を適用、`validation-routing.ts` が検証の振り分け、
  `stale-detector.ts` が陳腐化した仮説を `HypothesisStaleFlagged` する。

## ブリッジ (bridge/)

議論 ↔ 知識グラフをつなぐ層:

- **`bridge/gap/`** — 発話群から designGap を見つける。`detector.ts` (検出) +
  `matcher.ts` (既存 gap との突合) + `dedup.ts` (重複排除) + `affect-negatives.ts`
  (ネガティブ感情を論点化) + `manual.ts` (手動起票)。
- **`bridge/translation/`** — 議論の結論を KG の mechanic / aesthetic 等へ「翻訳」する。
  `pipeline.ts` + `review-queue.ts` / `review-handler.ts` (承認フロー) +
  `similarity.ts` (重複検出) + `llm-client.ts`。

## 射影と入力 (projection/)

`projection/` は外部入力をイベントへ落とすアダプタ:
`message-input.ts` (発話取り込み) / `command-parser.ts` + `command-handlers/`
(コマンド) / `reaction-handler.ts` / `responds-to-inference.ts` (返信先推定)。

## バックグラウンドジョブ (jobs/)

- `gap-detection-job.ts` — 溜まった発話から論点を定期抽出
- `hotspot-aggregation-job.ts` — 盛り上がり (hotspot) 集計 → `cache/hotspot-rank.ts`
- `stale-detection-job.ts` — 陳腐化仮説のフラグ立て
- `uncategorized-clustering-job.ts` — 未分類ノードのクラスタリング → `cache/uncategorized-clusters.ts`

## ベクトル検索 (vectors/)

`embedding.ts` でノード埋め込みを登録、`vector-search.ts` で `searchSimilar`
(workspace + nodeType 絞り込み + top-k)。gap matcher / translation similarity が利用。

## 関連

- 議論の自走 (停滞打開・収束) → `spec/feature/facilitator/DESIGN.md`
- persona の返信ループ → `spec/feature/persona-engine/DESIGN.md`
- Discord 入口・認証境界 → `CLAUDE.md` (認証モデル)
