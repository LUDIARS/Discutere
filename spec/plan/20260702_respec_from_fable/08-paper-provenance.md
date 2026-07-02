# 08 — ペーパー: 出所ラベル + 仮説メカニクス分離 + 証拠の RAG 分離

対応指摘: **C1** (LLM 増補メカニクスの無ラベル混入)・**A13** (debater 証拠レス、の入口整理)。
フェーズ: **PR-D**。

## 問題

- `mechanic-extract.ts` の増補メカニクス (感想から LLM 抽出、目標 30 件) が、curated な
  `data/games` 由来メカニクスと**同格・無ラベル**でペーパーに載る (`formatMechanics` は区別しない)。
  議論の共通地盤が未検証の生成物で汚染され、その上の定立・反定立・事実照会の信頼性が崩れる。
- ペーパーを「厚く盛る」方向 (paperRichness) に寄っており、注意の希釈を招く。
  証拠はペーパーに詰めるのではなく、必要時 (事実対立の解消・意見屋の参照) に RAG で引く分担が正しい。

## 設計

### 1. メカニクスの出所ラベル

```ts
export interface MechanicSummary {
  // 既存フィールドは不変
  source?: "curated" | "llm" | "crawl";   // 追加。undefined = 旧データ = curated 扱い
}
```

- `investigate.ts` (data/games 読込) → `curated`、`mechanic-extract.ts` (LLM 増補) → `llm`、
  クロール取込経路 (`learning.ts` / crawler) → `crawl`。
- **DB migration 不要** (mechanics_json 内のフィールド追加。旧行は undefined → curated 扱いで読む)。

### 2. ペーパー md の節分離

`paper-markdown.ts` の `paperDraftToMarkdown` / `markdownToPaperDraft` を改訂:

```markdown
# ゲームのメカニクス          ← source=curated / crawl (出所併記)
- **ガチャ**: ... → 期待感情: ... （出所: curated）

# 仮説メカニクス (LLM抽出・未検証)   ← source=llm
- **天井システム**: ... （根拠: 感想 n 件からの抽出）
```

- `buildPaperSystem` (= 各 LLM の system) にもこの節構造がそのまま載る。ペルソナは
  「仮説メカニクスは未検証」という前提を持って議論できる。
- round-trip (md ⇄ 構造化) は節名で source を復元。ブロックエディタ (Web) では
  仮説メカニクスも通常ブロックとして編集・削除・根拠クロール可能 (既存機能がそのまま効く)。

### 3. 増補の位置づけ変更

- `paperRichness.enrichMechanics` の既定は **true のまま**とするが、増補分は必ず
  `source: "llm"` で仮説節に入る (「量を盛る」から「仮説を明示的に提案する」への意味変更)。
- レビューゲート (paper-review) の表示で仮説節にバッジを出し、人間が議論開始前に
  仮説メカニクスを削れる (既存のブロック削除で可能。追加 UI 不要)。

### 4. 証拠の RAG 分担 (方針の明文化)

- ペーパー本文には感想の生データを増やさない (現行の userOpinionsText 配布ルール —
  意見屋/ローカル LLM のみ — は**維持**)。
- debater への証拠供給は dialectic エンジンの**事実対立解消** ([06](./06-dialectic-core.md) [3])
  が担う (tension 単位のオンデマンド RAG)。rounds エンジンでは現状維持
  (= 本設計書では debater 配布を変えない。入口の整理のみ)。
- `flow.paperRichness.voices` の既定 15 は据え置き (意見屋の参照値)。

## 実装ファイル

| ファイル | 変更 |
|---|---|
| `src/flow/investigate.ts` | MechanicSummary.source 付与 (curated) |
| `src/flow/mechanic-extract.ts` | 増補分に source=llm |
| `src/flow/paper-markdown.ts` | 仮説メカニクス節の書き出し/読み戻し (round-trip) |
| `src/flow/discussion-paper.ts` | `formatMechanics` の出所併記 |
| `src/flow/paper-review.ts` / `flow/web/page.ts` | 仮説節バッジ表示 |

## テスト / 受け入れ基準

- [ ] curated と llm 増補が md 上で節分離され、round-trip で source が保存される
- [ ] 旧 mechanics_json (source なし) が curated として読める (後方互換)
- [ ] buildPaperSystem の出力に「仮説メカニクス (LLM抽出・未検証)」節が含まれる
- [ ] 増補 0 件のとき仮説節自体が出ない (空節を作らない)
- [ ] `npm run test:phase3` (translation) / paper round-trip 既存テスト + 新規 `tests/flow/paper-provenance.test.ts`
