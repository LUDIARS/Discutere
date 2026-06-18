# LLM コスト relay (コスト表示面へ Push)

Discutere の LLM コスト (`llm_call_log`) を **コスト表示面**へ集約する Push 経路。
受信口は複数あり、同じ集計を**複数サービスへ同時 Push** できる:

- **Anatomia**: `POST /api/cost-feed`(Anatomia リポ参照)
- **Concordia**: `POST /v1/cost-feed`(Concordia リポ参照、Anatomia パネルの複製)

`anatomiaUrl` / `concordiaUrl` の両方を設定すると、1 回の集計を両サービスへ書き込む
(push 先複数化)。エンドポイントのパスはサービスごとに異なる(`COST_FEED_PATHS`)。

## 何を送るか

`costFeedRows()`(`src/flow/cost-report.ts`)が `llm_call_log` を
**(session_id × model × backend) 別**に集計した行を送る。各行はそのセッションの
**累積**サマリ(calls / input・output tokens / cache read・creation / cost_usd)。

受信側はいずれも `(service, sessionId, model, backend)` で **latest-wins dedupe** するので、
同じセッションを再 Push しても二重計上されず、最新の累積値で置き換わる。

> `cost_usd` は claude-cli(サブスク)では**等価 API 換算の推定**(実課金ではない)、
> anthropic 従量経路は raw API が返さず通常 0。

## 経路

- **定期**: サーバ起動時に `startCostRelay`(`src/flow/cost-relay.ts`)の timer が**常駐**する。
  各 tick で実効設定(enabled / 送信先 / service)を runtime-settings から読み直し、有効かつ
  URL が 1 つ以上ある時だけ Push する(= 設定 UI のライブ変更が次 tick に効く)。各 tick は
  「前回から 1 interval 重ねた時刻以降に活動したセッション」を `activeSince` で選び、
  そのセッションの累積を**全 target へ** Push(取りこぼし回避 / delta ではない)。
  カーソル前進は全 target 成功時のみ — 一部失敗した window は次 tick で再送する
  (累積 Push なので idempotent)。無効中は cursor を near-now に保ち、有効化した瞬間に
  過去 backlog を一気に送らない。送信失敗は議論を止めない。
- **手動/cron**: `npm run cost-relay`(`scripts/cost-relay.ts`)。
  `--url`(Anatomia) / `--concordia-url`(Concordia) / `--since` / `--service`。

## 設定 (`cost.relay`)

| key | env | 既定 | 説明 |
|---|---|---|---|
| `enabled` | `DISCUTERE_COST_RELAY_ENABLED` | `false` | 定期 relay を有効化(URL も 1 つ以上必要) |
| `anatomiaUrl` | `DISCUTERE_COST_RELAY_URL` | — | Anatomia ベース URL(例 `http://localhost:4200`)→ `/api/cost-feed` |
| `concordiaUrl` | `DISCUTERE_COST_RELAY_CONCORDIA_URL` | — | Concordia ベース URL(例 `http://localhost:17330`)→ `/v1/cost-feed` |
| `intervalMs` | `DISCUTERE_COST_RELAY_INTERVAL_MS` | `300000` | 定期 Push 周期 |
| `service` | `DISCUTERE_COST_RELAY_SERVICE` | `discutere` | cost-feed の service ラベル |

`anatomiaUrl` と `concordiaUrl` は独立。片方だけ・両方どちらでも可。両方設定すれば両方へ送る。

### 設定 UI でライブ管理 (`/api/admin/tuning`)

`enabled` / `anatomiaUrl` / `concordiaUrl` / `service` は **議論チューニング UI**(`/api/admin/tuning`、
loopback)から再起動なしで編集できる。値は runtime-settings(SQLite `discutere-settings.db`)に
override として永続し、`startCostRelay` が毎 tick `resolve()` で読み直すため次 tick に反映される。
config(`cost.relay.*`)は UI override の**デフォルト**(UI 未編集なら config どおり)。
`intervalMs`(timer 周期)だけは起動時固定で UI 対象外(変更は再起動)。

- API: `GET /api/admin/tuning/data`(現在値)/ `PUT /api/admin/tuning/cost-relay`
  (`{enabled?, anatomiaUrl?, concordiaUrl?, service?}` 部分更新)。

## worker-pool 経路の usage 回収 (#135)

worker-pool backend(常駐 Lictor ワーカー)は utterance callback で usage を返さない。
代わりに **transcript から token を拾って callback に載せる**:

1. ワーカー spawn 時に `LICTOR_PIN_TRANSCRIPT=1` を渡す(`worker-pool/spawner.ts`)。
2. Lictor は Concordia 無効でも `--session-id` でセッションを固定し、その transcript JSONL
   の絶対パスを `LICTOR_TRANSCRIPT_FILE` として wrapped Claude の env に公開する(Lictor `wrap.ts`)。
3. ワーカーが 1 ターンごとに `node scripts/send.mjs` を実行する際、`worker-home/scripts/usage.mjs`
   が transcript の assistant `message.usage` のうち**前回 send 以降に増えた分(デルタ)**を合算し、
   callback body の `usage` に載せる。デルタはカーソル(`<transcript>.di-usage-cursor`)で持ち越し、
   各 assistant 行を一度だけ計上する(`llm_call_log` は SUM 集計なので累積を送ると二重計上になる)。
4. `/internal/worker/utterance` が `usage` を受け、`WorkerPool.onUtterance` → `WorkerPoolClient` →
   `LLMResult.usage` → `withCostLog` が `llm_call_log` に記録 → cost-relay が各表示面へ。

## cost_usd の補完(サブスクでもコストを出す)

claude-cli 経路は envelope の `total_cost_usd` を持つが、**worker-pool / anthropic 経路は token
しか得られず cost_usd が NULL** になり、サブスク横断のコスト比較ができなかった。
`src/persona-engine/llm/pricing.ts` の `estimateCostUsd(model, usage)` が **token × モデル単価**で
等価 API コストを推定し、`withCostLog` が cost_usd 未取得時に補完する:

- 単価は Anthropic の公開 list price(opus $5/$25・sonnet $3/$15・haiku $1/$5 /MTok)に揃えてあり、
  claude-cli の `total_cost_usd`(= 等価 API 換算)と**同じ基準なので直接比較できる**。
- cache は Claude Code が打つ 5 分 ephemeral 前提(write=入力×1.25 / read=入力×0.1)。
- claude-cli は envelope 値を優先(推定で上書きしない)。単価表に無いモデル(ローカル LLM 等)は
  NULL のまま(ローカルは実質無料)。
- いずれも**実課金ではなく等価 API 換算の推定値**(サブスクは定額、実制約は rate-limit クォータ)。

### 残る制約

- **transcript は token のみ**: worker-pool の cost_usd は transcript の token から**推定**した値
  (claude-cli のような Anthropic 算出の envelope 値ではない)。比較は同一単価基準で成立するが、
  Anthropic の内部計算と完全一致は保証しない。
- **per-turn lag**: send 実行時点で当該ターンの usage 行が未フラッシュなら、そのデルタは次回 send で
  拾う(落とさないがセッション末尾の最終ターンが 1 つ遅れることがある)。
- worker-pool は config 既定 OFF。未起動時は claude-cli フォールバックがコスト計装済み。
