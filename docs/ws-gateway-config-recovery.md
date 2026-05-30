# Di: WS(Gateway)型再設計 + config ファイル化 + restart-recovery

2026-05-30 / feat/ws-gateway-config-recovery

前回セッション (`session-logs/2026-05-30.md`) の「次セッション」3 件を 1 PR で実装する。
役割は議論専用 (Discatier)。Chat-to-Task は Iv へ移管済み (MACHINA 層は本 PR の対象外)。

## 1. WS(Gateway)型再設計

### 背景 / 動機
- 旧 transport は **HTTP Interactions Endpoint** (`POST /api/discord/interactions`)。
  - 公開 URL が必須 (Discord が外から叩く)。
  - リクエスト毎に Ed25519 署名検証が必要。
  - ローカル/半ローカル運用 (Discord ギルド内 ChatBot) には公開 URL が重い。
- **Discord Gateway (WebSocket) に常時接続**すれば:
  - 公開 URL 不要 (bot からアウトバウンドで張る)。
  - 接続は **bot token で認証** → リクエスト毎の署名検証が不要。
  - `INTERACTION_CREATE` / `MESSAGE_CREATE` を push で受け取れる。
- `docs/discatier-discord-hook-architecture.md` の Next Steps #1「Bot Gateway 追加」をここで実装。
  - normalize は既に Gateway 形 (`{t:"MESSAGE_CREATE", d:{...}}`) 対応済 → transport を足すだけ。

### 採用: discord.js
姉妹サービス Concordia (`src/discord/bot.ts`) と同じ `discord.js` Client を使う。
heartbeat / resume / reconnect / shard を自前実装せず堅牢性を担保 (decision-metrics:
作業コスト低 × 解決度高 × 主目的一致高)。

### transport / ドメイン分離 (SRP)
旧 `src/api/discord-routes.ts` に混在していた「HTTP 受け口」と「slash → Discatier
ルーティング」を分離する。

- `src/discord-hook/command-router.ts` … **transport 非依存**のドメインロジック。
  - `routeSlashCommand(InboundSlashCommand, deps): SlashReply`
    - `discutere-kill` / `discutere-status` → persona-engine 制御 (admin-id allowlist)。
    - それ以外 → `/<name> <args>` に組み立て `submitMessage` へ流す。
  - `routeInboundMessage(DiscordInboundMessage, guildId, deps)`
    - bot 以外の平文メッセージを discord-bound session の utterance として記録。
- `src/discord-hook/gateway.ts` … discord.js Client (WS transport) の薄い adapter。
  - `Events.InteractionCreate` → `routeSlashCommand` → `interaction.reply`。
  - `Events.MessageCreate` → normalize → `routeInboundMessage`。

### 撤去するもの
- HTTP `POST /api/discord/interactions` ルート登録 (= `src/api/discord-routes.ts`)。
- インバウンド経路の Ed25519 署名検証 (`verifyAndParseInteraction` を呼ぶ箇所)。
  - ※ ライブラリ関数 `verifyDiscordSignature` / `interactions.ts` のパーサ群は**残す**
    (Slack HMAC と共用 / テスト資産 / 将来 HTTP fallback 用)。撤去は「経路」のみ。

### 認証境界の移動 (CLAUDE.md と整合)
- 旧: interaction の Ed25519 署名 + admin-id allowlist。
- 新: **Gateway 接続が bot token 認証**。admin slash の認可は引き続き
  `discord.adminIds` allowlist (未設定なら全 deny の安全 default)。

## 2. config ファイル化

env 散在を `src/config.ts` の単一 typed config に集約。

- 読み込み優先順: **default < config ファイル < env**
  (秘密情報/CI は env で上書き可能、平常運用は JSON ファイル)。
- ファイルパス: `DISCUTERE_CONFIG` (既定 `./discutere.config.json`)。無ければ env/default。
- 例: `discutere.config.example.json`。

集約対象 (Discatier 議論系):
`server.port/frontendUrl`, `jwtSecret`, `workspace`, `discatier.kuzuPath`,
`personaEngine.{dbPath,maxFiresPerSession,maxFiresPerRule,tickMs,bridgePollMs}`,
`llm.{backend,anthropicApiKey,model,claudeCliTimeoutMs,gitBashPath}`,
`discord.{botToken,applicationId,guildId,adminIds}`。

LLM 構造は Memoria (`server/llm.ts`) の backend/model/bin/git_bash_path 方式を参考に
した (ただし Discutere は自前 LLMClient 実装を保持、Memoria settings DB には結合しない)。

MACHINA 系 env (HAIKU_MODEL / GITHUB_TOKEN / LUDUS_*) は Iv 移管対象のため本 config に
含めない (現状維持)。

## 3. restart-recovery

### 結論 (前回レビューの確定)
Discatier は Event Sourcing。ドメインは projection 再構築で再取得可能。
**揮発で要対処は persona-engine の per-session fire counter のみ** (`engine.ts` の
`sessionFires = new Map()`)。`last_fired_at` (cooldown) は既に rules テーブルに永続化
されているので再起動耐性あり。Redis 不要・SQLite で完結。

### 実装
- `sessionFires: Map<sessionId, Map<ruleId, count>>` を `SessionFireStore` interface に抽象化。
  - `InMemorySessionFireStore` (既定 / テスト用、従来挙動)。
  - `SqliteSessionFireStore` (本番)、新テーブル `session_rule_fires(session_id, rule_id, count)`。
- migration `pe_0002_session_fires` を追加 ([[feedback_sqlite_create_index_after_alter]] に従い
  INDEX は CREATE TABLE と同 exec で OK・新規テーブルなので ALTER 順序問題なし)。
- `createPersonaEngine` が同一 db で `SqliteSessionFireStore` を配線。
- 効果: 再起動しても session 内の総発火/ルール別発火カウントが復元され、safety cap
  (`maxFiresPerSession` / `maxFiresPerRulePerSession`) が再起動越しに効く。
  `resetSession` (議論クローズ) で DB 行も削除。

## テスト
- `tests/persona-engine/session-fire-store.test.ts` — in-memory / sqlite 両 store の
  increment/total/ruleCount/reset + **再起動再現** (同 db で新 store を作って復元確認)。
- `tests/discord-hook/command-router.test.ts` — engine slash (kill/status/admin allowlist)
  と Discatier command ルーティングを transport 非依存で検証。
- 既存 `tests/discord-hook/interactions.test.ts` は維持 (パーサ群は残すため)。
- gateway.ts は discord.js 実接続を要するため unit 対象外 (薄い adapter)。

## 運用メモ
- slash command の Discord 登録 (application commands upload) は別運用 (本 PR は transport)。
  Gateway 接続後、登録済みコマンドの `INTERACTION_CREATE` を受ける。
- `discord.botToken` 未設定なら Gateway は skip 起動 (HTTP health 等は従来通り)。
