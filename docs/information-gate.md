# 情報ゲート (Information Gate) — 議論前の情報密度評価フェーズ

## 目的

Di の議論は「収集済みの外部の声 (プレイヤー/視聴者の生の感想)」を材料にする。材料が薄い
(声が少ない / 観点が偏る) まま議論を始めると、ペルソナが憶測で喋り **議論の精度が落ちる**。

旧来の自動学習 (`flow.autoCrawl` / `ensureLearningData`) は「外部の声が `minVoices` 件未満なら
クロールする」という **単純カウント閾値** で、情報の密度・観点の多様性を一切見ていなかった。

情報ゲートは、議論/改善フローの開始直前に **LLM が情報密度を評価** し、不足していれば
**不足観点を狙って学習 (クロール) → 再評価** する自己改善ループを挟む。十分になり次第そのまま
議論を開始する (**自動モード固定**)。

## フロー

```
議論/改善フロー開始の直前 (gateBeforeFlow):

  ① listExternalVoices(theme) で既存材料を集める
        │
  ② evaluateInformationDensity (LLM 1 コール)
        │   → { density: sparse|moderate|rich, covered[], gaps[] }
        │     gaps = [{ aspect, reason, query, source? }]
        │
  ③ density >= minDensity ?  ── yes ──→ 十分。 そのまま議論へ
        │ no
  ④ 不足観点 (gaps) を maxGapsPerIteration 件まで取り、
     各 query で collectAndImport (config 既定ソースでクロール → KG 取込)
        │
  ⑤ 再評価 (②へ)。 十分 or 反復 maxLearnIterations 回で打ち切り
        │
  ⑥ 届かなくても sufficient=false で議論を開始 (degrade、止めない)
```

- **自動モード固定**: 不足なら自動で学習し、十分になり次第そのまま議論を開始する。人間確認は挟まない
  (headless 自走前提)。Discord では学習が走ったときだけスレッドに `📊 …` で進捗を通知する。
- **degrade**: LLM 障害 / JSON 解析失敗のときは、外部の声の **件数だけ** で粗く密度を見積もる
  fallback に倒し、議論は止めない (`fallbackDensity`: ≥8→rich / ≥3→moderate / それ未満→sparse)。
- **コスト上限**: 1 議論あたり LLM 評価は最大 `maxLearnIterations + 1` 回、クロールは最大
  `maxLearnIterations × maxGapsPerIteration` 回。既定 (iter=2, gaps=2) で評価 3 回・クロール 4 回が上限。

## 実装

| ファイル | 役割 |
|---|---|
| `src/flow/information-gate.ts` | ゲート本体 (`evaluateInformationDensity` / `runInformationGate`)。LLMClient を直接呼ぶ。DB に触れないので単体テスト可能。 |
| `src/flow/information-gate-runner.ts` | 呼び出し側グルー (`gateBeforeFlow`)。config 解決 + Core open/close + `withCostLog` ラップ + 対象フロー判定。 |
| `src/flow/external-voices.ts` | `makeListExternalVoices` — adapter の `listRelevantExternalVoices` から `listExternalVoices` を組み立てる配線ヘルパ (これまで web/discord 経路で未配線だった T7 follow-up)。 |
| `src/flow/learning-autocrawl.ts` | `collectAndImport` を抽出・公開 (充足ゲート無しで「取得→取込」だけ行う)。`ensureLearningData` はこれを使うよう内部リファクタ (外部契約は不変)。 |
| `src/flow/web/routes.ts` | `prepareInformationBeforeFlow`: ゲート優先 → 対象外なら旧 autoCrawl にフォールバック。 |
| `src/flow/discord-live.ts` | `prepareInformationBeforeForumFlow`: 同上 + 学習時のスレッド通知。 |
| `src/index.ts` | `setFlowWebDeps` / `flowLive` に `listExternalVoices` を配線。 |
| `src/config.ts` | `flow.informationGating` 設定を追加。 |

## 設定 (`flow.informationGating`)

| キー | 既定 | env | 説明 |
|---|---|---|---|
| `enabled` | `true` | `DISCUTERE_FLOW_INFOGATE_ENABLED` | false で旧 autoCrawl (カウント閾値) にフォールバック |
| `minDensity` | `moderate` | `DISCUTERE_FLOW_INFOGATE_MIN_DENSITY` | これ以上の密度で「十分」(sparse<moderate<rich) |
| `maxLearnIterations` | `2` | `DISCUTERE_FLOW_INFOGATE_MAX_ITERATIONS` | 不足時の学習反復上限 |
| `maxGapsPerIteration` | `2` | `DISCUTERE_FLOW_INFOGATE_MAX_GAPS` | 1 反復で狙い撃ちする不足観点数 (= クロール回数上限) |
| `model` | `""` | `DISCUTERE_FLOW_INFOGATE_MODEL` | 評価モデル ("" なら LLM 既定) |

クロールのソース / `maxItems` は `flow.autoCrawl` の設定を流用する (既定 niconico = テーマ検索・キー不要)。
LLM が `gaps[].source` を推奨しても、実クロールは config 既定ソースを使う (steam=appId / youtube=API キー
等の事故を避けるため。推奨ソースは記録のみ)。

## 観測

- 評価の LLM コールは `llm_call_log` に `location="information-gate"` で記録される (`withCostLog`)。
- 進捗は `log` (web=`[flow-gate]` / discord=`[forum-gate]`) と Discord スレッド通知 (`📊 …`) に出る。

## スコープ外 (follow-up 候補)

- **DiscussionPaper への評価メタ埋め込み**: 評価結果 (density / gaps) をペーパーに載せて、ペルソナ prompt に
  「この観点は手薄」と伝える。現状は深い plumbing と migration を避けてログ + 通知に留めた。
- **ソース横断学習**: 現状は config 既定ソース 1 種でしか学習しない。gaps[].source を活かして
  niconico/youtube/steam/website を観点ごとに使い分ける (キー/appId 解決が要る)。

## 密度判定のルール卒業 (成長型ブラックボックス, 2026-07-02)

密度評価は `@ludiars/blackbox` (LUDIARS/Lapilli、設計正本 packages/blackbox/DESIGN.md) 経由になった
(`src/flow/density-blackbox.ts`、runner が `evaluateFn` に注入)。

- LLM 評価時に「この判定は閾値で再現できるか」を同じ 1 コールで聞き、Condition 候補
  (feature: voiceCount / sourceKinds / avgLen / tagCount) を candidate として蓄積する。
- candidate は発火せず、以後の評価のたびに LLM 判定と影で突合 (影評価)。一致 3 回で trial
  (発火 + レビュー待ち)、admin の OK×3 で auto = **卒業 (この密度判定に LLM を呼ばなくなる)**。
- ルール経路では gaps (不足観点クエリ) は返らないが、runInformationGate は gaps 空のとき
  theme をクエリにクロールするので従来どおり回る (degrade)。
- レビュー/管理は `/api/admin/blackbox/*` (decisions キュー / verdict / rules+卒業メトリクス /
  state 手動変更)。テーブル (blackbox_rules / blackbox_decisions) は discutere.db 上に
  パッケージの ensureBlackboxSchema が作成する (flow migration 体系の外)。
