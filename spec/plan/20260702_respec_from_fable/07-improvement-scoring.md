# 07 — 改善フロー: 効果予測ベクトルによるスコアリング再設計

対応指摘: **A3** (文面の感情 ≠ 採用効果のカテゴリエラー)。フェーズ: **PR-C** (独立)。
正本 spec: [improvement.md](../../feature/flow/improvement.md) (本 respec で改訂)。

## 問題

現行スコア `projection(textToVector(意見文) − currentVec, designGap)` は
「意見を採ったときの感情の動き」を測ると謳うが、`textToVector` が測るのは
**意見テキスト自体に表出している感情**。結果:

- 熱いポジティブな文面の意見が内容と無関係に高スコア (文面のポジティブさコンテスト)
- 辞書 substring 一致は 1〜2 文の日本語口語でほぼヒットせず、全意見が同点付近 → 実質先着勝ち
- アスペクト次元は「言及あり → 文全体 valence を適用」で、賛否混在の意見が正しく写像されない
- `loadCurrentVector` が日本語テーマで一致せず黙って negative ベースラインに落ちる

## 設計

### 1. 効果予測 (LLM) → design_gap 射影

「意見の文面」ではなく「**意見を採用した場合に体験がどう動くか**」を LLM に予測させ、
その**予測変化量**を design_gap に射影する。20 次元空間と射影の枠組みは維持:

```
predictEffect(意見, theme, mechanics) → 構造化 JSON:
{
  "changes": [ { "dim": "asp.fun", "delta": 0.4, "reason": "..." }, ... ],  // -1..1、言及次元のみ
  "confidence": "high | low"
}
score(意見) = scalarProjection(changesToVector(changes), designGap)
```

- `changesToVector`: 言及のない次元は 0 (移動なし)。既存 `VECTOR_DIMS` の次元名をそのまま
  プロンプトに列挙し、モデルには**変化しそうな次元だけ**挙げさせる (全次元強制はノイズ)。
- モデル: `flow.improvement.effectModel` (既定 Haiku 級)・温度 0。1 意見 1 call
  (ラウンドあたり +ターン数 call。improvement は投票 LLM を使わないため純増はほぼ相殺)。
- withCostLog location="effect-predict"。

### 2. 失敗時 degrade

- LLM 失敗 / パース失敗: 当該意見のみ**旧 lexicon 方式**で採点し、`improvement_score` に
  `method` 列 (下記) で `"lexicon-fallback"` を記録 + warn。全滅時も議論は完走 (OVERVIEW §10)。

### 3. スキーマ (migration。列追加のみ)

```sql
ALTER TABLE improvement_score ADD COLUMN method TEXT NOT NULL DEFAULT 'lexicon';
  -- 'effect-predict' / 'lexicon-fallback' / 'lexicon' (旧データ)
ALTER TABLE improvement_score ADD COLUMN detail_json TEXT;  -- changes[] (監査・可視化用)
```

### 4. currentVector の解決改善

- `loadCurrentVector` のテーマ照合に既存 `game-aliases.ts` (`expandAliases`) を使い、
  日本語タイトル・略称でも `.sentiment.json` に届くようにする。
- **negative ベースラインへの fallback を warn ログで明示** (現在は無言)。

### 5. スコープ外 (明記)

- 20 次元空間の再定義はしない (SENTIMENT.md 準拠)。
- 議論フロー (中立投票) には効果予測を導入しない (世論は投票のまま、improvement.md の分担維持)。

## 実装ファイル

| ファイル | 変更 |
|---|---|
| `src/flow/effect-predict.ts` (新規) | predictEffect + changesToVector + degrade |
| `src/flow/improvement.ts` | evaluator を effect-predict 経由に、method/detail 永続 |
| `src/flow/improvement-score.ts` | scoreOpinions が「予測変化ベクトル」を受ける形にシグネチャ変更 (射影は不変) |
| `src/flow/design-gap.ts` | loadCurrentVector に alias 展開 + fallback warn |
| `src/config.ts` | `flow.improvement.effectModel` |

## テスト / 受け入れ基準

- [ ] Mock: 「熱い文面だが効果が負」の意見が「冷静な文面で効果が正」の意見より低スコア (カテゴリエラーの回帰テスト)
- [ ] LLM 失敗 → lexicon-fallback で採点され method 列に記録、議論完走
- [ ] 「モンスト」テーマで `モンスターストライク.sentiment.json` が引ける (alias 展開)
- [ ] sentiment.json 不在時に warn + negativeBaseline (無言 fallback の解消)
- [ ] 既存 `tests` の improvement 系 + 新規 `tests/flow/effect-predict.test.ts`
