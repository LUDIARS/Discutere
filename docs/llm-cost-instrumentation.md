# LLM コスト計装 (claude-cli サブスク経路 + 横断集計)

サブスク (Claude Code OAuth) で `claude -p` を spawn する経路でも、API 経路と同様に
トークン使用量・プロンプトキャッシュ usage・コスト推定を記録できるようにした計装。

## 背景

`claude -p` の stdout を plain text で受けていたため、`claude-cli` / `worker-pool`
backend では `llm_call_log.input_tokens` / `output_tokens` が常に NULL だった
(コスト・キャッシュ効率を後から検証できない)。

サブスクは per-token のドル課金が無い定額制なので「実課金額」は存在しないが、
`claude -p --output-format json` のエンベロープは **`total_cost_usd`(等価 API 換算の
推定値)** と **`usage`(`cache_read_input_tokens` / `cache_creation_input_tokens`
含む)** を OAuth でも返す。これを拾って記録する。

## 変更点

1. **`claude-cli.ts`** — spawn を `claude -p --output-format json` に変更。stdout を
   `parseClaudeCliResult()`(純関数・named export)でパースして `result`(本文) +
   `usage` + `total_cost_usd` を抽出。CLI バージョン差 (単一オブジェクト / イベント配列
   末尾 `type:"result"`) の両方に対応。`is_error` / `subtype!=="success"` / パース失敗 /
   本文空は `ok:false`。
2. **`persona-engine/types.ts`** — `TokenUsage` に `cache_read_input_tokens` /
   `cache_creation_input_tokens` / `cost_usd`(全 optional)を追加。
3. **`anthropic.ts`** — レスポンス usage 型を広げ、cache トークンも素通しで記録
   (この backend は既に `cache_control` を付与済み。cost_usd は raw API が返さないので NULL)。
4. **DB マイグレーション `flow_0010_llm_cost`** — `llm_call_log` に
   `cache_read_input_tokens` / `cache_creation_input_tokens` / `cost_usd` を ALTER ADD
   (新規 INDEX 不要、冪等)。
5. **`cost-logger.ts`** — `withCostLog` が上記 3 列も INSERT。
6. **`cost-report.ts` + `scripts/cost-report.ts`(`npm run cost-report`)** —
   `llm_call_log` をセッション別 / フロー別 / backend 別に集計。

## 使い方

```
npm run cost-report                       # 全期間の合計 + backend/flow/session 別
npm run cost-report -- --session <id>     # 単一セッション
npm run cost-report -- --since <epochMs>  # created_at >= since
npm run cost-report -- --json             # JSON 出力
```

## 注意

- **`cost_usd` は実課金ではない**。claude-cli (サブスク) は等価 API 換算の推定値、
  anthropic 従量経路は raw API が返さないため通常 NULL=0。サブスクの実制約は
  rate-limit クォータ。
- **`worker-pool` backend は未対応**。常駐 Lictor ワーカーの utterance callback が
  usage を返さないため、トークン/コストは NULL のまま。Lictor 側でワーカー応答に usage を
  載せる対応が必要 (follow-up)。
- 第三者ツールへの OAuth トークン供与は ToS 違反 (2026-01 fingerprint 遮断)。本計装は
  公式 `claude` バイナリ自身が吐く json を読むだけで、トークンを外部に渡さない。
