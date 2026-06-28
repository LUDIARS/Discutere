# Discutere

**遊びの議論プラットフォーム (Discatier)** — ゲームデザインの「意図された体験 (`intended_affect`)」と
「観測された体験 (`expressed_affect`)」を 3 軸の弁証法的対話で突き合わせ、設計のズレ (`DesignGap`) と
跳躍的仮説 (`Hypothesis`) を育てる **Discord-only の自走議論 ChatBot**。

> **アーキテクチャ (canonical / 2026-05-30 WS Gateway 再設計)**
> - **transport = Discord Gateway (WebSocket) 常時接続**。公開 URL / HTTP Interactions Endpoint /
>   Ed25519 署名検証は撤去済み。bot token でアウトバウンド接続するので外部公開不要。
> - **認証 = bot token + admin-id allowlist**。Cernere / 独自 JWT / React frontend は撤去済み
>   (半ローカルツールゆえの意図的設計)。詳細は `CLAUDE.md`。
> - 設定は `src/config.ts` の単一 typed config に集約 (`default < discutere.config.json < env`)。
>
> 旧 *Chat-to-Task 自動化 (Slack/Discord → タスク生成)* 機能群 (`src/machina/`) は **撤去済み**
> (2026-06-28)。同等のタスク自動化は Concordia (RWF / Session-End) が担うため、Discutere は
> 議論プラットフォーム (Discatier) に専念する。

## コンセプト — Discatier の 3 軸対話

| 軸 | モード | 役割 | 生成物 |
|---|---|---|---|
| Axis 1 学習対話 | `learning` | 語彙 / メカニクスの定義を精緻化 | `Mechanic.intended_affect` |
| Axis 2 感情会話 | `emotion` | プレイヤーが理論用語を介さず生の感情を報告 | `expressed_affect`(観測) |
| Axis 3 統合 | `synthesis` | Axis 1↔2 のズレに仮説を提示・検証 | `DesignGap` → `Hypothesis` |

単一のデータ基盤 (Kuzu グラフ + イベントログ) 上で Translation Bridge / Gap Detection /
Hypothesis Lifecycle が弁証法的ループを形成する。詳細は [`docs/discatier_implementation_plan.md`](docs/discatier_implementation_plan.md)。

## Tech Stack

| Layer | Technology |
|-------|-----------|
| Runtime | Node.js 22+ / TypeScript (ESM) |
| Discord | [discord.js](https://discord.js.org/) v14 — Gateway (WS) 常時接続 |
| HTTP (admin/dashboard) | [Hono](https://hono.dev/) + `@hono/node-server` (port 3110) |
| データ基盤 | Discatier Core = Kuzu(SQLite) + イベントログ / サイドカー SQLite (Drizzle) |
| LLM | Anthropic SDK 直叩き or Claude CLI (Lictor 経由 spawn) |
| 設定 | `src/config.ts` 単一 typed config (`default < discutere.config.json < env`) |
| バックアップ | tar.gz → S3 (`@aws-sdk/client-s3`、Glacier 系ストレージクラス) |

## Quickstart (Discord で議論を最短で動かす)

```sh
# 1. clone + 依存 + build
git clone https://github.com/LUDIARS/Discutere && cd Discutere
npm install
npm run build

# 2. config を用意 (実体は gitignore)
cp discutere.config.example.json discutere.config.json
#   discord.botToken / discord.applicationId / discord.guildIds / discord.adminIds を埋める
#   議論を取り込むチャンネルは discord.discussionChannelIds に列挙
#   秘密情報 (botToken / apiKey) は env 上書き推奨

# 3. 起動 (Gateway 常時接続 + persona-engine attach + slash 自動登録)
npm start
#   → :3110 listen、Discord にログイン、slash command を自動登録
```

### Discord 側の準備

1. <https://discord.com/developers/applications> で Application + Bot を作成
2. **Bot Token** / **Application ID** を控える
3. Privileged Gateway Intents の **Message Content** を有効化 (平文取り込みを使う場合)
4. OAuth2 URL Generator で `bot` + `applications.commands` scope + 投稿権限を選び guild に invite
   - Gateway 接続なので **公開 URL / INTERACTIONS ENDPOINT URL の設定は不要**

### slash command の登録

起動時 (`gateway` の ClientReady) に **自動登録**される。`guildIds` 指定時は **guild commands**
として即時反映、未指定時は **global commands** (反映に最大 1 時間)。bot を起動せず手動登録したい場合:

```sh
npm run discord:register   # discord.botToken + discord.applicationId が必要
```

> 旧バージョンは登録コードが無く、slash がクライアントに一切出ない不具合があった (#この PR で修正)。

## 使える slash command

| command | 説明 | 認可 |
|---|---|---|
| `/propose statement:<仮説>` | hypothesis を提案 → persona の自走議論が走る | 全員 |
| `/validate mode:theory\|emotion` | hypothesis を検証 | 全員 |
| `/integrate` | 検証済 hypothesis を統合 (採用) | 全員 |
| `/reject` | hypothesis を棄却 | 全員 |
| `/discutere-status` | persona-engine の稼働状態 (ephemeral) | 全員 |
| `/discutere-queue` | 議論キュー (進行中 session / 未処理 gap / 検証待ち仮説) を可視化 | 全員 |
| `/discutere-kill enabled:true\|false` | 自走を停止/再開 | admin only |
| `/discutere-backup` | 学習データを今すぐ S3 にバックアップ | admin only |

## 自然なテキスト取り込み

`discord.discussionChannelIds` に入れたチャンネルでは、**slash を打たなくても全平文を議論
(utterance) として自然に取り込む**。

- 取り込んだメッセージには bot が 👀 リアクションを付け、「議論に乗った」ことをフィードバックする。
- **スレッド**内の発言は、親チャンネルが許可リストにあれば継承して取り込む。AI の返信も同じ
  スレッド/チャンネルに返る (session.scene が発言先に bind するため)。
- allowlist 外のチャンネルは取り込まない (ノイズ混入を防ぐ安全 default)。

## 複数サーバ (guild) 対応

複数 Discord サーバの議論を **1 つの workspace (= 単一 Discatier KG) に集約**できる。

- `discord.guildIds` に運用 guild を列挙する (各 guild に slash を登録 + 状態カードを設置)。
- 学習データ (Kuzu KG) は `discatier.kuzuPath` 単一に集約される。session は
  `discord:<guildId>/<channelId>` で guild 別に分離されるが、KG は 1 つ。
- bot は「1 トークンを複数 guild に invite」「guild ごとに別 bot」のどちらでも動く
  (各 monitor 行が自身の botToken を持ち、post 時は guild ID 一致で bot を選ぶのでクロス post しない)。

## 議論キューの可視化

「今どんな議論が走っていて、何が積まれているか」を可視化する。

- **slash**: `/discutere-queue` — 進行中 session / 未処理 DesignGap / 検証待ち Hypothesis のサマリ
- **dashboard**: `GET /api/admin/dashboard` の「🧭 議論キュー」カード (5 秒 polling、admin role)
- **API**: `GET /api/admin/queue` — スナップショット JSON (active session・open gap・pending hypothesis)

## 学習データの S3 バックアップ

Discatier KG (kuzu) + persona-engine.db + discutere.db を **tar.gz にまとめて S3** に push する
(`storageClass=GLACIER` 系で安価に長期アーカイブ。MinIO 等の S3 互換にも `endpoint` で対応)。

- **自動 (月次)**: `backup.enabled=true` かつ `backup.bucket` 設定で、`intervalDays` (既定 30 日)
  経過ごとに自動バックアップ。最終実行は `backup.stateFile` に記録され、再起動を跨いで判定する。
- **手動 (script)**: `npm run backup`
- **手動 (slash)**: `/discutere-backup` (admin only) — 起動して即時 ack、結果はサーバログ / dashboard で確認

```jsonc
// discutere.config.json (抜粋)
"backup": {
  "enabled": true,
  "bucket": "my-discutere-archive",
  "region": "ap-northeast-1",
  "prefix": "discutere/",
  "storageClass": "GLACIER",     // DEEP_ARCHIVE / STANDARD_IA / STANDARD も可
  "intervalDays": 30,            // 月次
  "endpoint": ""                 // MinIO 等の S3 互換 (任意)
}
```

> 認証情報は env (`AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY`) または AWS SDK 既定チェーン
> (IAM ロール等) を使う。値は config に直書きせず env 上書きを推奨。

## セットアップガイド (用途別)

「○○するための設定」は [`spec/setup/`](spec/setup/) に用途別でまとめてある:

| やりたいこと | ガイド |
|---|---|
| Discord で議論を動かす (最短) | [spec/setup/discord.md](spec/setup/discord.md) |
| AI に自走議論させる (LLM backend) | [spec/setup/llm.md](spec/setup/llm.md) |
| 複数サーバ (guild) で運用する | [spec/setup/multi-server.md](spec/setup/multi-server.md) |
| 学習データを S3 にバックアップする | [spec/setup/backup.md](spec/setup/backup.md) |
| **全設定キーを確認する** | [spec/setup/config-reference.md](spec/setup/config-reference.md) |

設定は `default < discutere.config.json < env` の順で解決 (`DISCUTERE_CONFIG` で config パス変更可)。
雛形は [`discutere.config.example.json`](discutere.config.example.json) /
[`.env.example`](.env.example)。秘密情報は env 上書き推奨。

## 開発

```sh
npm run dev:server   # tsx watch (ホットリロード)
npm run build        # tsc → dist/
npm start            # node dist/index.js
```

### テスト

```sh
npm run test:core            # Discatier Core (Phase 0)
npm run test:phase2          # message projection
npm run test:phase3          # translation bridge
npm run test:phase4          # gap detection
npm run test:phase5          # hypothesis lifecycle
npm run test:phase6          # cross-axis projection
npm run test:persona-engine  # 議論駆動エンジン
npm run test:discord-hook    # Gateway / command-router / interactions
npm run test:queue           # 議論キュー snapshot
npm run test:backup          # バックアップ archive
npm run test:crawler         # KG クローラー
npm run test:visualize       # magic-link md 書き出し
```

## persona-engine (議論駆動)

```sh
npm run persona-demo            # MockLLM デモ (API 不要)
$env:ANTHROPIC_API_KEY="sk-ant-..."; npm run persona-demo -- --real-llm   # 実 LLM
```

エンジンは `src/persona-engine/` に閉じており、切り出し時は `mv src/persona-engine
../persona-engine-package/src/` + `package.json` 分割で完結する設計。Discatier 接続は
`src/discatier-engine-adapter/` に隔離。per-session 発火カウンタは `session_rule_fires`
テーブルに永続化され、再起動を跨いで safety cap が効く。

## Game KG クローラー / 議論ソース可視化

```sh
npm run crawl import data/games/sample-hollow-knight.md   # md を Discatier Core に取り込む
npm run crawl list                                        # 登録済 Game/Mechanic/Aesthetic

npm run visualize hypothesis <id>   # data/discussions/<ws>/hypothesis/<id>.md に書き出し
npm run visualize gap <id>          # gap / mechanic / aesthetic / utterance / session も同様
```

ノード間参照は `[[hyp:abc123]]` / `[[utt:550e8400]]` 形式の magic-link md。詳細は
[`spec/feature/crawler/DESIGN.md`](spec/feature/crawler/DESIGN.md) / [`spec/feature/visualize/DESIGN.md`](spec/feature/visualize/DESIGN.md)。

## ディレクトリ構成 (抜粋)

```
src/
├── index.ts                  # エントリ (Hono + Gateway + engine + backup wiring)
├── config.ts                 # 単一 typed config (default < json < env)
├── discord-hook/             # Discord Gateway transport
│   ├── gateway.ts            #   discord.js Client 常時接続 + slash 自動登録 + 👀 ack
│   ├── command-router.ts     #   transport 非依存ルーティング (slash + 平文)
│   ├── command-defs.ts       #   slash command 定義 (single source of truth)
│   ├── register-commands.ts  #   REST PUT で Discord に登録
│   ├── discussion-bridge.ts  #   AI 発話を guild/channel に post
│   └── poster.ts             #   Discord channel への投稿
├── core/                     # Discatier Core (Event Sourcing + Kuzu projection)
├── persona-engine/           # 議論駆動エンジン (ペルソナ × ルール × LLM)
├── queue/snapshot.ts         # 議論キューのスナップショット生成
├── backup/                   # tar.gz アーカイブ + S3 upload + 月次スケジューラ
└── api/                      # admin / dashboard / queue ルート

scripts/
├── register-discord-commands.ts   # npm run discord:register
└── backup.ts                       # npm run backup
```

## License

[MIT](LICENSE) © LUDIARS
