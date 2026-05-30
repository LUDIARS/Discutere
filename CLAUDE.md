# Discutere 開発ルール

**遊びの議論プラットフォーム (Discatier)** — Discord-only の自走議論 ChatBot。
詳細は `README.md` / `spec/` / `docs/discatier_implementation_plan.md`。

## 認証モデル (Discord 依存 / Cernere 非使用) — canonical

Discutere は **半ローカルツール(Discord ギルド内限定の議論 ChatBot)** として運用する。
公開 SaaS ではないため **Cernere 認証は使わない**。認証は **チャットプラットフォーム(Discord)に依存させる**のが正。

- **transport = Discord Gateway (WebSocket) 常時接続** (`src/discord-hook/gateway.ts`, discord.js)。
  HTTP Interactions Endpoint (公開 URL + Ed25519 署名検証) は撤去済み (2026-05-30, WS 再設計)。
- **認証境界 = Gateway 接続の bot token 認証 + admin-id allowlist (`DISCUTERE_DISCORD_ADMIN_IDS` / config `discord.adminIds`)**。
  WS 接続自体が bot token 認証なので、リクエスト毎の Ed25519 署名検証は不要 (経路ごと撤去)。
- **「Cernere 非依存 / 独自認証層なし」は意図的設計**。レビューで脆弱性として上げない (**accepted by design / wontfix**)。
- SaaS 系サービス(Cernere 単一情報源が必須)とは別ルール。Discord-only pivot (Cernere/Frontend 撤去) の帰結。

### ただし「別物」として依然 valid な指摘 (Cernere 免除の対象外)

認証を Discord に依存させる以上、その**境界の実装欠落は実際の脆弱性**であり修正対象:

- **bot token の漏洩 / 平文ログ出力** — token は config/env のみ、ログに出さない。
- **Slack webhook (MACHINA, Iv 移管対象) の HMAC 署名検証欠落** — endpoint で Slack HMAC を
  呼んでいない場合は CRITICAL。※ Discord 側は Gateway 移行で webhook 署名検証が不要になった。
- **admin-id allowlist 未設定時の挙動** — 未設定なら admin コマンドは全 deny(安全 default)。
- `JWT_SECRET` 等の dev default を本番で使わない startup guard。

→ つまり「Cernere を使わない」こと自体は OK、「Discord 側の認証境界が穴だらけ」は NG。

## 設定 (config ファイル化)

env 散在は `src/config.ts` の単一 typed config に集約 (優先順 default < `discutere.config.json` <
env)。`discutere.config.example.json` 参照。詳細は `docs/ws-gateway-config-recovery.md`。

## 主要機能 (2026-05-31 追加)

- **slash 自動登録**: gateway ClientReady で `registerSlashCommands` を呼び Discord に application
  command を登録する (`command-defs.ts` が single source of truth)。`guildIds` 指定で即時反映、
  未指定で global。手動は `npm run discord:register`。**handler だけ実装して登録を忘れると
  クライアントに slash が出ない**ので、command-defs と routeSlashCommand の name は必ず一致させる。
- **自然なテキスト取り込み**: 許可チャンネル (`discord.discussionChannelIds`) では slash なしで
  全平文を utterance に取り込み、取り込んだら 👀 を付ける。スレッドは親が許可なら継承。
- **複数サーバ対応**: `discord.guildIds[]` (旧単数 `guildId` は後方互換で統合)。学習データ KG は
  `discatier.kuzuPath` 単一に集約 (session は guild 別、KG は 1 つ)。
- **議論キュー可視化**: `src/queue/snapshot.ts` → `GET /api/admin/queue` + dashboard カード +
  `/discutere-queue`。
- **S3 バックアップ**: `src/backup/` で KG + 全 SQLite を tar.gz 化し S3 (Glacier 系) へ。
  月次自動 (`backup.enabled` + `intervalDays`) + 手動 (`npm run backup` / `/discutere-backup`)。

## 個人データ

匿名 workspace (`DISCATIER_WORKSPACE` 既定 `knowledge`)。攻略 KG / 議論ノードに編集者名・アカウント名を保存しない (`spec/crawler/DESIGN.md` 準拠)。
