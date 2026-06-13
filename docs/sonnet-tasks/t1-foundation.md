# T1: 共通基盤 (設定 + タグ + DB スキーマ + コスト/transcript ログ)

設計参照: `spec/flow/OVERVIEW.md` §2 (設定) / §3 (タグ) / §8 (コスト計測)。

## ゴール

4 フローが乗る土台を作る。フロー共通設定・タグ体系・新規 DB テーブル・LLM コスト/transcript
ログ機構を用意する。後続タスク (T2 以降) はすべてこの上に乗る。

## スコープ

### in
1. **設定**: `src/config.ts` に `flow` セクションを追加。
   | キー | 既定 | 意味 |
   |---|---|---|
   | `flow.rounds` | 3 | 最大ラウンド数 |
   | `flow.turnsPerRound` | 6 | 1 ラウンドのターン数 |
   | `flow.personaCount` | 4 | 議論プレイヤー人数 |
   | `flow.voterCount` | 3 | 投票者数 (中立) |
   | `flow.tags` | `[]` | 既定タグ (テーマ単位で上書き) |
   | `flow.sparringMaxTurns` | 50 | 壁打ちの暴走ガード上限 (T6 で使用) |
   | `flow.youtubeMaxComments` | 200 | 0 件補完時の取得コメント上限 (T2 で使用) |
   - env override (`DISCUTERE_FLOW_*`) も既存 `pickNum`/`parseStringList` で実装。
   - 既存 `config.discussion.*` は**消さず併存** (モデル編成・ペース設定として残す)。

2. **タグ体系**: `src/flow/tags.ts` (新規) にタグの定義と判定ヘルパ。
   - タグ種別: `機密` `内部` `運用` `開発` (+ 任意拡張)。
   - `canCollectExternal(tags): boolean` — `機密` か `内部` を含むと `false` (最も厳しいタグが
     優先。OVERVIEW §3)。
   - `paperSupplement(tags): string` — 運用/開発の観点補足テキストを返す。
   - フォーラムタグ (`discord.forum.*`) とは別軸。混同しないこと。

3. **DB スキーマ** (`src/db/schema.ts` にマイグレーション追加。`discutere.db`):
   - `discussion_paper` — 1 議論 = 1 ペーパー。
     `(id, flow, session_id, theme, tags_json, mechanics_json, supplement, created_at, updated_at)`
   - `discussion_paper_round` — ラウンド追記 (append)。
     `(id, paper_id, round, summary, aufhebung_json, created_at)`
   - `vote` — 投票 (T3 で書込)。
     `(id, session_id, round, voter_index, chosen_utterance_id, created_at)`
   - `llm_call_log` — コスト + transcript を 1 テーブルに統合 (OVERVIEW §8、内部検証用)。
     `(id, flow, session_id, round, turn, role, persona, location, model, backend,
       latency_ms, input_tokens, output_tokens, prompt, response, created_at)`
     - `location` = 呼び出し種別 (`classifier` / `facilitator` / `utterance` / `vote` /
       `summary` / `sentiment`)。
     - `prompt` / `response` は全文 (内部のみ・外部非公開なのでマスク不要、OVERVIEW §8.3)。
   - **INDEX は ALTER の後**に冪等発行 (共通ルール)。

4. **コストログ機構**: `src/flow/cost-logger.ts` (新規)。
   - 既存 `LLMClient.invoke` (`src/persona-engine/llm/client.ts`) を**ラップ**し、呼び出しごとに
     `llm_call_log` へ 1 行書く高階関数 `withCostLog(client, ctx)` を提供。
   - `ctx` = `{ flow, sessionId, round, turn, role, persona, location }`。
   - `usage` (`TokenUsage { input_tokens, output_tokens }`) を永続化。返さない backend
     (claude-cli / worker-pool) は null 許容 (best-effort、OVERVIEW §8.3)。
   - latency は invoke 前後の経過 ms。

### out (やらない)
- FlowDirector 本体 (T2)。投票ロジック (T3)。WebUI / Discord 入口 (T7)。
- 既存テーブルの破壊的変更。

## 受け入れ条件 (テスト)
- `getConfig().flow` が既定値で解決し、env / config ファイルで上書きできる。
- `canCollectExternal(['機密'])===false`, `canCollectExternal(['開発'])===true`,
  `canCollectExternal([])===true`。
- マイグレーションが**新規 DB と既存 DB の両方**で boot エラー無く適用される (既存 DB =
  カラム追加 → INDEX が後)。
- `withCostLog(mockClient, ctx)` で invoke すると `llm_call_log` に 1 行入り、token/latency/
  prompt/response が記録される。usage 無し backend では token が null。

## 関連
- `src/config.ts` / `src/db/schema.ts` / `src/persona-engine/llm/client.ts` /
  `src/persona-engine/types.ts` (`TokenUsage`)。
- メモリ: SQLite migration INDEX 順 / boolean serialize 漏れ (README 共通ルール参照)。
