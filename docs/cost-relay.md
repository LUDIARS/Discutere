# LLM コスト relay (Anatomia コスト削減UIへ Push)

Discutere の LLM コスト (`llm_call_log`) を **Anatomia のコスト削減UI**へ集約する Push 経路。
Anatomia 側の受信口は `POST /api/cost-feed`(Anatomia リポ参照)。

## 何を送るか

`costFeedRows()`(`src/flow/cost-report.ts`)が `llm_call_log` を
**(session_id × model × backend) 別**に集計した行を送る。各行はそのセッションの
**累積**サマリ(calls / input・output tokens / cache read・creation / cost_usd)。

Anatomia は `(service, sessionId, model, backend)` で **latest-wins dedupe** するので、
同じセッションを再 Push しても二重計上されず、最新の累積値で置き換わる。

> `cost_usd` は claude-cli(サブスク)では**等価 API 換算の推定**(実課金ではない)、
> anthropic 従量経路は raw API が返さず通常 0。

## 経路

- **定期**: サーバ起動時、`cost.relay.enabled` かつ `cost.relay.anatomiaUrl` が
  揃っていれば `startCostRelay`(`src/flow/cost-relay.ts`)が回る。各 tick は
  「前回から 1 interval 重ねた時刻以降に活動したセッション」を `activeSince` で選び、
  そのセッションの累積を Push(取りこぼし回避 / delta ではない)。送信失敗は議論を止めない。
- **手動/cron**: `npm run cost-relay`(`scripts/cost-relay.ts`)。`--url` / `--since` / `--service`。

## 設定 (`cost.relay`)

| key | env | 既定 | 説明 |
|---|---|---|---|
| `enabled` | `DISCUTERE_COST_RELAY_ENABLED` | `false` | 定期 relay を有効化(URL も必要) |
| `anatomiaUrl` | `DISCUTERE_COST_RELAY_URL` | — | Anatomia ベース URL(例 `http://localhost:4200`) |
| `intervalMs` | `DISCUTERE_COST_RELAY_INTERVAL_MS` | `300000` | 定期 Push 周期 |
| `service` | `DISCUTERE_COST_RELAY_SERVICE` | `discutere` | cost-feed の service ラベル |

## 既知の制約

- **worker-pool backend のコストはまだ NULL**(常駐 Lictor ワーカーが usage を返さない)。
  Lictor 改修で transcript から usage を回収 → callback に載せる対応が必要(follow-up)。
  それまで relay には anthropic / claude-cli 経路のコストのみ乗る。
