# T5: 学習フロー (議論前のユーザ意見/メカニクス収集)

設計参照: `spec/flow/learning.md`。依存: **T1**。

## ゴール

ゲームタイトル + テーマに即した**ユーザの意見 / メカニクス**を発話・記録するフロー。
**LLM による議論はしない**。議論フローの前段で素材を仕込む。

## スコープ

### in
1. **収集フロー** (`src/flow/learning.ts`):
   - 入力: ゲームタイトル + テーマ。
   - **ユーザ意見 (感想)** と **メカニクス** を収集・記録する (議論・ラウンド・投票は無し)。
   - 既存の**ゲーム感想チャンネル** (`discord.gameFeedback`、カテゴリ「ゲーム感想」) の匿名収集を
     流用候補とする。
   - 感情極性付与は感情カスケード (`src/crawler/sentiment/cascade.ts`) を流用。
   - 記録先: メカニクス → Discatier Core `mechanics` / `data/games/<slug>.md`、意見 → KG/コーパス
     (個人データ非保管、`CLAUDE.md` 準拠)。

2. **議論フローとの接続**:
   - 学習結果が議論フロー step 2 (調査) / step 3 (ペーパーのユーザ意見まとめ) の入力になることを
     確認する (同じ KG を読む)。

### out
- LLM 議論 (このフローでは一切しない)。投票・ラウンド。
- 収集 UI の作り込み (既存チャンネル流用で可)。

## 受け入れ条件 (テスト)
- 学習フローがユーザ意見とメカニクスを記録し、議論フローの調査がそれを読める。
- LLM 議論 (ペルソナ発話 / ラウンド) が**呼ばれない**ことを確認。
- 個人データ (author 名等) が保存されない。

## 関連
- `spec/feature/game-feedback.md` / `discord.gameFeedback` / `src/crawler/sentiment/cascade.ts` /
  `src/core/repositories/mechanic.ts`。
