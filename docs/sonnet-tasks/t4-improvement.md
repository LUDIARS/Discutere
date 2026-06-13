# T4: 改善フロー (design_gap 機械スコア)

設計参照: `spec/flow/improvement.md` / `spec/flow/OVERVIEW.md` §11-c。依存: **T2, T3**。

## ゴール

議論フローと同骨格の「改善フロー」を追加する。差分は **step 6 の世論決定を LLM 投票でなく
機械計算 (design_gap への射影) にする** 点のみ。

## スコープ

### in
1. **フロー登録**: `flow='improvement'` として FlowDirector を再利用 (T2/T3 の骨格をそのまま使う)。
   step 1〜5・7〜9 は議論フローと同一。

2. **design_gap 計算** (`src/flow/design-gap.ts`):
   - **現状 (起点)** ベクトル = Di 基礎感情データ (`src/crawler/sentiment/`)。**negative 想定**。
   - **目標** ベクトル = メカニクスの `intended_affect` (Discatier Core schema)。
   - **design_gap = 目標 − 現状 = positive な改善ベクトル**。

3. **機械スコアリング** (`src/flow/improvement-score.ts`, step 6 置換):
   - 各意見について、その意見を採ったときの感情ベクトルの動きを計算。
   - スコア = **「positive な改善ベクトル (design_gap 方向) が結果として現れるか」**。
     - design_gap への**射影 (内積)** が正で大きいほど高スコア。
     - **「近さ (cosine)」ではない**。negative→positive の**移動量**を見る (improvement.md 実装メモ)。
   - **最高スコアの意見 = そのラウンドの世論 (主要意見)**。改善フローでは**中立投票を行わない**。
     機械スコアのみで世論を確定する (議論フロー T3 の投票フェーズは**実行しない**)。
   - LLM を使わない計算部分はコストログ不要。感情ベクトル算出に LLM カスケードを使う場合のみ
     `location='sentiment'` で記録。

### out
- 議論フロー骨格の再実装 (T2/T3 を再利用)。新しい感情空間の定義 (既存 20 次元を使う)。

## 受け入れ条件 (テスト)
- design_gap が `目標 − 現状` で計算され、想定 negative 起点 + positive 目標で正方向ベクトルになる。
- improvement-score が design_gap への射影でスコア付けし、**正射影が大きい意見が勝つ**
  (cosine 近さでなく移動量で勝つケースをテストで区別)。
- 改善フローで世論が機械スコアで確定する (LLM 投票に依存しない)。
- 議論フローと改善フローが、同一入力で**世論決定ロジックだけ異なる** (改善は機械スコア・投票なし)
  ことを確認。
- 改善フローで T3 の中立投票が**呼ばれない**ことをテストで確認。
