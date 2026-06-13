# T2: 議論フロー — FlowDirector + ディスカッションペーパー + 調査/YouTube

設計参照: `spec/flow/discussion.md` step 1〜5 / `spec/flow/OVERVIEW.md` §4 (駆動) / §5 (ペーパー)
/ §6 (RAG)。依存: **T1**。

## ゴール

議論フローの**駆動とラウンド/ターン進行**、**ディスカッションペーパー生成**、**調査
(メカニクス+感情, 0 件時 YouTube 補完)** までを実装する。投票・結論は T3。

## スコープ

### in

1. **FlowDirector** (`src/flow/director.ts` + 補助ファイル。SRP で分割):
   - persona-engine は使わない。**ターン駆動**の同期ループ。
   - 議論開始時に **`flow.personaCount` 人のペルソナをその場生成** (使い捨て・永続しない)。
     ロール: ファシリテーター ×1 + 残りを賛成派/反対派/意見屋に割当。**賛否はターン時に決定**。
   - ラウンドループ: `flow.rounds` 回。各ラウンド `flow.turnsPerRound` ターン。
   - **各ターン: その場生成ペルソナからランダムに 1 人選ぶ** → そのペルソナに発言要求。
   - 各ターンで渡すのは **3 つだけ**: ペルソナ (人格+スタンス) / ディスカッションペーパー /
     当ラウンドの意見。空応答は発話に数えない。エラーは発話に出す (T1 cost-logger 経由で
     `location='utterance'` 記録)。
   - ファシリテーターがラウンド開始時にテーマ出題。
   - **prompt-builder 変更**: コンテキストを**当ラウンド優先スコープ**にする
     (`src/persona-engine/engine/prompt-builder.ts` を本フロー用に分岐 or 新規 builder)。

2. **ディスカッションペーパー** (`src/flow/discussion-paper.ts`):
   - 生成: 共通項目 = テーマ + メカニクス + タグ補足 (T1 `paperSupplement`) + 前ラウンド結果。
   - **ロール別配布** (OVERVIEW §5.2):
     - 賛成派 / 反対派 → 共通項目のみ
     - 意見屋 → + ユーザ意見まとめ (YouTube/Steam のランダム抽出)
   - **ローカル LLM 特例** (§5.3): `llm.backend==='local'` のペルソナはロール問わず RAG
     (ユーザ意見ランダムチョイス) を付与。= 賛成派/反対派の「何も渡さない」を上書き。
   - **機密特例** (§5.4): `機密` タグ時は実ユーザ意見の代わりに**想定意見データ** (T2 では
     intended_affect からの簡易合成でよい。`(synthetic)` ラベル付き、実音声と混ぜない)。
   - RAG 実体は既存 `listRelevantExternalVoices` (出所付き・個人仮名) を流用。
   - ペーパーは T1 の `discussion_paper` に永続。ラウンドサマリ追記は T3 が
     `discussion_paper_round` へ。

3. **調査** (`src/flow/investigate.ts`, `spec/flow/discussion.md` step 2):
   - テーマ関連の**メカニクス** (Discatier Core `mechanics` / `data/games/<slug>.md`) と
     **感情データ** (`*.sentiment.json` / `affects` / `cascade.ts`) を集める。
   - **0 件時 YouTube 補完** (step 2-1):
     - `canCollectExternal(tags)` が `false` (機密/内部) なら**やらない**。
     - 取得前に**ユーザへ警告**を出す (「感情データが無いので YouTube を検索します」等)。
     - 取得は **100〜200 コメント** (`flow.youtubeMaxComments`)。既存 YouTube 取込
       (task #128 インポート / `youtube-comments` skill) を流用。
     - 取得結果は感情カスケード (`cascade.ts`) で極性付与してから利用。

4. **テスト用トリガ** (最小):
   - dev 用に `FlowDirector.start(theme, tags)` を直接叩ける関数 or 一時 slash。正式入口は T7。

### out
- 投票 / 感情ベクトル評価 / ラウンドサマリ / 結論 (T3)。
- 正式な WebUI / Discord 入口 + フロー選択 (T7)。
- 想定意見データの高度な合成 (T2 は簡易で可、精緻化は follow-up)。

## 受け入れ条件 (テスト, MockLLM)
- `FlowDirector.start(theme)` で `flow.rounds × flow.turnsPerRound` のターンが回り、各ターンの
  発話が永続する (MockLLM 固定応答)。
- 各ターンのペルソナ選択がランダム (シードを変えて分布確認、または選択関数の単体テスト)。
- ディスカッションペーパーが生成され、ロール別に配布内容が変わる (意見屋のみユーザ意見入り /
  local backend ペルソナは賛成派でもユーザ意見入り / 機密はユーザ意見が `(synthetic)`)。
- 感情データ 0 件 + 非機密で YouTube 取得が呼ばれ (mock)、機密/内部では呼ばれない。
- YouTube 取得前に警告メッセージが出る。
- LLM 失敗時にエラーが発話に出て、`llm_call_log` にも記録される。

## 関連
- `src/persona-engine/engine/prompt-builder.ts` (流用/分岐) / `src/crawler/sentiment/cascade.ts`
  / `src/core/repositories/mechanic.ts` / `listRelevantExternalVoices` (外部の声 RAG) /
  `spec/feature/discussion-party.md` (旧 composition、置換対象)。
