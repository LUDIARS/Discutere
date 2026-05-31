# 設定リファレンス (config / env 全キー)

Discutere の全設定キーの正本。用途別の手順は [README.md](README.md) の各ガイドへ。

## 解決順序

```
default < discutere.config.json < env
```

- config ファイルパスは env `DISCUTERE_CONFIG` で変更可 (既定 `./discutere.config.json`)。
- 実装は [`src/config.ts`](../../src/config.ts) の単一 typed config。
- 雛形は [`discutere.config.example.json`](../../discutere.config.example.json) /
  [`.env.example`](../../.env.example)。
- **秘密情報 (botToken / anthropicApiKey / AWS 鍵) は config に直書きせず env 上書きを推奨**。

## server

| config | env | 既定 | 説明 |
|---|---|---|---|
| `server.port` | `BACKEND_PORT` | `3100` | HTTP (admin / dashboard / queue) port |
| `server.frontendUrl` | `FRONTEND_URL` | `http://localhost:5174` | CORS 許可 origin |
| `workspace` | `DISCATIER_WORKSPACE` | `knowledge` | 匿名議論 workspace (= KG 集約単位) |

## discatier (Core KG)

| config | env | 既定 | 説明 |
|---|---|---|---|
| `discatier.kuzuPath` | `DISCATIER_KUZU_PATH` | adapter 既定 | Discatier Core KG のパス |

## personaEngine (自走の安全弁)

| config | env | 既定 | 説明 |
|---|---|---|---|
| `personaEngine.dbPath` | `DISCUTERE_PERSONA_ENGINE_DB` | `./data/persona-engine.db` | engine の SQLite |
| `personaEngine.maxFiresPerSession` | `PERSONA_ENGINE_MAX_FIRES_PER_SESSION` | `20` | session 総発火上限 |
| `personaEngine.maxFiresPerRule` | `PERSONA_ENGINE_MAX_FIRES_PER_RULE` | `5` | 同一 rule 発火上限 |
| `personaEngine.tickMs` | `PERSONA_ENGINE_TICK_MS` | `5000` | engine tick 周期 |
| `personaEngine.bridgePollMs` | `PERSONA_ENGINE_BRIDGE_POLL_MS` | `2000` | events polling 周期 |

→ 詳細は [llm.md](llm.md)。

## llm

| config | env | 既定 | 説明 |
|---|---|---|---|
| `llm.backend` | `LLM_BACKEND` | `anthropic` | `anthropic` / `claude-cli` / `mock` |
| `llm.anthropicApiKey` | `ANTHROPIC_API_KEY` | — | anthropic backend 用 |
| `llm.model` | `ANTHROPIC_MODEL` | client 既定 (Haiku) | モデル ID |
| `llm.claudeCliTimeoutMs` | `CLAUDE_CLI_TIMEOUT_MS` | `120000` | claude-cli の応答 timeout |
| `llm.gitBashPath` | `CLAUDE_CODE_GIT_BASH_PATH` | — | **Windows + claude-cli で必須** |

→ 詳細は [llm.md](llm.md)。

## discord

| config | env | 既定 | 説明 |
|---|---|---|---|
| `discord.botToken` | `DISCUTERE_DISCORD_BOT_TOKEN` | — | Gateway 接続 (空なら skip) |
| `discord.applicationId` | `DISCUTERE_DISCORD_APPLICATION_ID` | 自動解決 | slash 自動登録 |
| `discord.guildIds` | `DISCUTERE_DISCORD_GUILD_IDS` (カンマ区切り) | `[]` | 運用 guild 群 |
| `discord.guildId` (後方互換) | `DISCUTERE_DISCORD_GUILD_ID` | — | 旧 単数。`guildIds` に統合 |
| `discord.adminIds` | `DISCUTERE_DISCORD_ADMIN_IDS` (カンマ区切り) | `[]` | admin slash 認可 (空=全 deny) |
| `discord.discussionChannelIds` | `DISCUTERE_DISCORD_DISCUSSION_CHANNELS` (カンマ区切り) | `[]` | 平文取り込み ch (空=取り込まない) |

→ 詳細は [discord.md](discord.md) / [multi-server.md](multi-server.md)。

## backup (S3)

| config | env | 既定 | 説明 |
|---|---|---|---|
| `backup.enabled` | `DISCUTERE_BACKUP_ENABLED` | `false` | 月次自動の有効化 |
| `backup.bucket` | `DISCUTERE_BACKUP_BUCKET` | — | S3 bucket |
| `backup.region` | `DISCUTERE_BACKUP_REGION` / `AWS_REGION` | `ap-northeast-1` | バケットの region |
| `backup.prefix` | `DISCUTERE_BACKUP_PREFIX` | `discutere/` | key prefix |
| `backup.storageClass` | `DISCUTERE_BACKUP_STORAGE_CLASS` | `GLACIER` | ストレージクラス |
| `backup.endpoint` | `DISCUTERE_BACKUP_ENDPOINT` | — | S3 互換 (MinIO 等) |
| `backup.intervalDays` | `DISCUTERE_BACKUP_INTERVAL_DAYS` | `30` | 自動周期 (月次) |
| `backup.stateFile` | `DISCUTERE_BACKUP_STATE_FILE` | `./data/backup-state.json` | 最終実行記録 |
| (認証) | `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` | — | 未指定なら SDK 既定チェーン |

→ 詳細は [backup.md](backup.md) / [`../backup/DESIGN.md`](../backup/DESIGN.md)。

## その他 (MACHINA — Iv 移管予定)

`HAIKU_MODEL` / `GITHUB_TOKEN` / `DATABASE_PATH` 等は Chat-to-Task (MACHINA) 系。
議論 (Discatier) 運用では不要。`DATABASE_PATH` (既定 `./data/discutere.db`) は
バックアップ対象には含まれる。
