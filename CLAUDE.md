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
  全平文を utterance に取り込む。👀 リアクションは**取り込み全件ではなく「議論の種(開始エントリ)に
  なった投稿」=auto-discussion が designGap を新規に立てた時だけ**付ける (リアクション=議論が立った
  合図 / persona-engine の返信と対応)。スレッドは親が許可なら継承。
- **複数サーバ対応**: `discord.guildIds[]` (旧単数 `guildId` は後方互換で統合)。
- **タスク別 KG (2026-06-08)**: 学習データ KG は **タスク (収集目的) ごとに分割**できる
  (`discatier.knowledgeGraphs[]` レジストリ + `activeKg` で起動時に 1 つ選択、 切替は再起動)。
  旧 `discatier.kuzuPath` 単独指定は id=`default` の 1 KG として後方互換で吸収。Core を開く全箇所は
  `resolveActiveKgPath(config)` 経由で active KG を見る。無停止ホットスワップは非対応
  (`spec/crawler/EXTERNAL-SOURCES.md` §12)。
- **議論キュー可視化**: `src/queue/snapshot.ts` → `GET /api/admin/queue` + dashboard カード +
  `/discutere-queue`。
- **S3 バックアップ**: `src/backup/` で KG + 全 SQLite を tar.gz 化し S3 (Glacier 系) へ。
  月次自動 (`backup.enabled` + `intervalDays`) + 手動 (`npm run backup` / `/discutere-backup`)。

## フォーラム集約 (2026-06-06, `discord.forum`)

議論を Discord **フォーラムチャンネル** に集約する (`docs/forum-aggregation.md`)。フォーラムを
「議論カテゴリ」として使い、guild 内の **全 Forum チャンネル** を監視する。

- **入口**: フォーラム新規ポスト (`ThreadCreate`, 親=`GuildForum`) の **最初の投稿 (starter)**
  で議論を起こす (`forum-monitor.handleForumThreadCreate` → `command-router.routeForumPost`)。
  scene を `discord:<guild>/<threadId>` に紐付けるので、AI 返信・収束まとめはそのスレッドに出る。
  starter 判定は `msg.id === thread.id`。後続投稿は進行中議論への参加発言 (新規 gap は立てない)。
- **議論の方向性 (タグ)**: フォーラムポストの適用タグで方向を決める。「改善提案」系タグ →
  課題抽出+改善案、「面白さ」系タグ → 魅力の語り合い。**タグ無しは既定で「面白さ」方向**。
  方向は gap 説明にディレクティブとして差し込まれ facilitator の拡張/収束を steer する
  (`forum-monitor.pickForumDirection` → `auto-discussion.forumDirectionDirective`)。
  タグ名は config `discord.forum.improvementTagNames` / `funTagNames` (部分一致) で調整可。
- **クローズ**: facilitator が収束し gap を closed にすると `onConverged` フックが発火 →
  gateway の `finalizeForumPost` がスレッドを **lock + archive** し、まとめを「まとめ投稿」へ転記。
- **自動作成チャンネル** (ClientReady, guild ごと、Manage Channels 権限が必要):
  - **データ学習依頼** — 貼られた URL を crawl に回す入口 (id は runtime の crawl 集合へ追加)。
  - **まとめ投稿** — 収束まとめの集約先。
- 既存 `discussionChannelIds` (平文議論) は後方互換で残すが、フォーラムが議論の正路。
  無効化は `discord.forum.enabled=false` (env `DISCUTERE_DISCORD_FORUM_ENABLED`)。

## 個人データ

匿名 workspace (`DISCATIER_WORKSPACE` 既定 `knowledge`)。攻略 KG / 議論ノードに編集者名・アカウント名を保存しない (`spec/crawler/DESIGN.md` 準拠)。

**外部発話の露出方針 (2026-06-08, `spec/crawler/EXTERNAL-SOURCES.md` §6)**: 出所メタ
(ソース種別 + 元 URL = attribution) は end user にも **開示**するが、 個人アンカー
(`authorId` 公開 ID / `authorName` 表示名) は露出面で **マスク**して内部ペルソナ表示名
(`論者#xxxx`) に置換する =「出所は透明 / 個人は仮名」。保管層はフル精度のまま (persona 学習用)、
出力 serializer が個人だけマスクし出所は透過する。生 ID 参照は admin のみ。
これは LUDIARS 利用者の個人データ (Cernere 単一情報源) とは別レイヤー
([[project_personal_data_rule]] の対象外 — 外部公開の仮名 platform ID)。

**外部の声 RAG (2026-06-08, §14)**: AI (persona-engine) が議論中に外部の生の声を**出所付きで
引用・参照**できる。FT は Claude の API/CLI に無いため RAG。`prompt-builder` が議題語で active KG の
外部発話を検索 (キーワード一致 + opinion-score 順、`listRelevantExternalVoices`) し、出所(source+URL)
付き・個人仮名で prompt に注入する。embedding 検索 / LLM 関連度判定 / FT は非採用 (follow-up)。
