# T3: 議論フロー — 投票 + 感情ベクトル評価 + ラウンドサマリ/止揚 + 結論

設計参照: `spec/feature/flow/discussion.md` step 6〜9 / `spec/feature/flow/OVERVIEW.md` §1 (投票/世論) / §11-b。
依存: **T1, T2**。

## ゴール

ラウンド終了後の **投票 (中立)**・**感情ベクトル評価 (補助)**・**ラウンドサマリ + 止揚追記**・
**結論** を実装し、議論フローを完成させる。

## スコープ

### in

1. **投票フェーズ** (`src/flow/vote.ts`, step 6):
   - ラウンド終了時、**`flow.voterCount` 人の中立投票者**で投票する。
   - 投票者は**ペルソナを注入しない** (中立)。テーマに最も的を射た「良い意見」を**当ラウンドの
     発話から 1 つ選ぶ** (utterance_id を返す)。各投票者 1 票。
   - 集計: **得票数 = 世論 = そのラウンドの主要意見**。`vote` テーブル (T1) に記録。
   - 既存のリアクションスコア (👀 + 人間下駄) は**使わない** (OVERVIEW §11-b)。
   - 投票も LLM 呼び出し → `withCostLog(..., location='vote')`。

2. **感情ベクトル評価** (`src/flow/sentiment-eval.ts`, step 6 補助):
   - 各意見の感情ベクトル (20 次元, `cascade.ts` / sentiment 空間) が、テーマ / 目標
     (メカニクス `intended_affect`) の**どのベクトルに近いか**、**ベクトルの近さ**で適合を判定。
   - 議論フローでは**補助シグナル** (世論は投票で決定)。結果はファシリテーターが保持
     (ペーパーには載せない)。
   - ※改善フローはここを design_gap 機械計算で**世論決定**に格上げする (T4)。共通化できる
     よう、ベクトル算出は再利用可能な関数に切る。

3. **ラウンドサマリ + 止揚** (`src/flow/round-summary.ts`, step 7):
   - ラウンドの意見を一度サマライズ (LLM, `location='summary'`)。
   - **止揚 (アウフヘーベン)** があればまとめる。既存 `judgeAndStockAufhebung` /
     `aufhebung_stock` のロジックを**関数として流用**。
   - サマリ + 止揚を **`discussion_paper_round` に追記** (T1)。次ラウンドの共通項目になる。

4. **次ラウンド / 結論** (step 8-9):
   - `flow.rounds` まで継続。新ラウンドは当ラウンド優先 (prompt-builder スコープ、T2 で実装済)。
   - **結論判定**: 既存 facilitator の収束ロジック (止揚数しきい値 / 収束まとめ) を関数流用。
     - 結論が出れば確定。出なければ「結論なし」。**全ラウンド完走前に出たら打ち切り**。
   - **まとめ整形は既存の結論ビュー `src/visualize/conclusions.ts` に倣う**。

### out
- 改善フローの design_gap 機械スコア (T4)。
- 学習/壁打ち (T5/T6)。入口 (T7)。

## 受け入れ条件 (テスト, MockLLM)
- ラウンド終了で `flow.voterCount` 票が投じられ、`vote` に記録、最多得票の utterance が世論
  として確定する (同数時の tie-break ルールも定義してテスト)。
- 投票者プロンプトにペルソナが入っていない (中立) ことを確認。
- 感情ベクトル評価が各意見にスコアを付け、テーマ/目標への近さで順位付けする。
- ラウンドサマリ + 止揚が `discussion_paper_round` に追記され、次ラウンドのペーパーに反映。
- 結論が出るケースで全ラウンド前に打ち切られ、出ないケースで「結論なし」になる。
- 結論まとめが conclusions.ts のビュー形式で整形される。
- 投票/要約失敗時にエラーが出る (握り潰さない) + `llm_call_log` 記録。

## 関連
- `src/persona-engine/facilitator/{facilitator.ts,prompts.ts}` (止揚/収束ロジック流用) /
  `src/crawler/sentiment/` (ベクトル) / `src/visualize/conclusions.ts` (結論ビュー)。
