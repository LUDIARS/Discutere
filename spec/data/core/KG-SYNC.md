# KG 共有同期 (マイグレーションシステム)

master ではない Di パッケージ (follower) が、master の **共有知識** を「特定の URL から差分
ダウンロード」して取り込む仕組み。 各サービスを跨いだ攻略/設計知識を 1 つの master に集約し、
複数の follower インスタンスへ配るための片方向レプリケーション。

## 1. 何を共有するか (共有資産)

学習/クロールで貯まる知識テーブルのうち、**`shared` フラグが立った行だけ**を配布する。

| テーブル | 内容 | 同期 |
|---|---|---|
| `games` / `mechanics` / `aesthetics` / `affects` | 学習知識 (ゲーム/メカニクス/美的/情緒) | ✅ `shared=1` の行のみ |
| `sessions` / `utterances` / `design_gaps` / `reactions` / `hypotheses` 等 | 各インスタンス固有の議論データ | ❌ 同期しない |

`shared` 列は migration `0006_kg_sharing` で追加 (既定 0)。master 側で 1 を立てた行が配布対象。
誰が立てるかは運用判断 (admin UI / 手動 UPDATE / 取り込み時の自動付与)。**配布対象は明示 opt-in**。

個人データは元々 KG に保存しない方針 (CLAUDE.md「個人データ」) なので、転送内容も仮名のまま。

## 2. データモデル

全共有テーブルは `id` (TEXT PK) と `updated_at` (INTEGER, epoch ms) を持つ。これにより
**増分 = `updated_at > watermark`**、**冪等 = `id` upsert** が自然に成立する。

## 3. 転送フォーマット (NDJSON)

```
{"type":"manifest","schemaVersion":1,"since":<n>,"watermark":<max updated_at>,"count":<n>,"tables":[...]}
{"type":"row","table":"games","data":{...全列...}}
{"type":"row","table":"mechanics","data":{...}}
...
```

1 行目 manifest の `watermark` を follower が保存し、次回 `since` に渡す。

## 4. master 側 (配信)

- **エンドポイント** `GET /api/kg/migrations?since=<epochMs>[&token=<secret>]`
  → `shared=1 AND updated_at>since` の行を NDJSON で返す (`src/api/kg-migration-routes.ts`)。
  `config.kgSync.serveToken` が設定されていれば `?token=` / `x-kg-token` を要求。
- **静的 publish** `npm run kg:export -- [--since N] [--out file]` で NDJSON ファイルを出力 →
  S3 / Pages 等へ置けば follower が静的 URL から GET 可能 (`scripts/kg-export.ts`)。

実装は `src/kg-sync/export.ts` (`exportSharedKg`)。

## 5. follower 側 (取り込み)

- **pull**: 設定 URL に `?since=<watermark>` を付けて GET → import → watermark 保存
  (`src/kg-sync/pull.ts`)。手動 `npm run kg:pull [URL] [--full]`、自動は `config.kgSync.auto`。
- **import**: `id` upsert + **last-write-wins** (受信 `updated_at` が既存以上のときだけ上書き)。
  whitelist 外テーブルは拒否、master/follower の列差は共通列のみ書く (`src/kg-sync/import.ts`)。
- **状態**: source URL ごとの watermark を `data/kg-sync-state.json` に保存 (`src/kg-sync/state.ts`)。

URL は master の生エンドポイントでも静的ファイルでも良い (どちらも GET→NDJSON)。静的ホストは
`since` を無視して全件返すが、import 側 LWW が冪等なので正しさは保たれる (転送量だけ増える)。

## 6. transport は URL 非依存

follower は「URL を GET して NDJSON を得る」だけなので、配信元が
master endpoint / 静的 publish のどちらでも同じ pull ロジックで動く。公開手段
(Cloudflare Tunnel / Tailscale / S3) は運用側の選択。

## 7. 設定 (`config.kgSync`)

| キー | env | 既定 | 用途 |
|---|---|---|---|
| `sourceUrl` | `DISCUTERE_KG_SYNC_URL` | — | follower が pull する URL |
| `token` | `DISCUTERE_KG_SYNC_TOKEN` | — | 配信元へ送る共有シークレット |
| `auto` | `DISCUTERE_KG_SYNC_AUTO` | false | 起動時+定期 pull |
| `intervalHours` | `DISCUTERE_KG_SYNC_INTERVAL_HOURS` | 6 | 定期 pull 周期 |
| `stateFile` | `DISCUTERE_KG_SYNC_STATE_FILE` | `./data/kg-sync-state.json` | follower 透かし |
| `serveToken` | `DISCUTERE_KG_SYNC_SERVE_TOKEN` | — | master endpoint の要求トークン |

## 8. Phase 2 (未実装)

- **embeddings**: `updated_at` を持たない (created_at のみ) + 再生成可能なので現状は対象外。
  必要なら follower 側で再 embed、または created_at 透かしで別系統同期。
- **外部発話 (Kuzu/sidecar)**: SQLite ではなく Kuzu グラフ + sidecar (`.ingested`/`.raw`/
  `.attribution`) に存在 (EXTERNAL-SOURCES.md)。別フォーマットが必要なため Phase 2。
- 削除伝播 (tombstone)・双方向同期・競合の field 単位マージは非対応 (片方向 + 行単位 LWW)。
