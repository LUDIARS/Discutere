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
- **進行役への調整指示 (2026-06-09, `docs/facilitator-directives.md`)**: Discord で bot (@Discutere)
  へメンションして「もっと簡単な言葉で」「もっと否定的に」のように **進行の調整**を出せる。
  persona への通常のリプライは参加発言のまま (調整は bot 明示メンションのみ)。
  監視対象 (フォーラムスレッド / `discussionChannelIds`) で検知し、
  通常の utterance 取り込みには回さず scene→進行中議論 (open な design gap) に紐付けて
  サイドカー表 `facilitator_directives` に保存 → 了解を一言リプライ。 以後 **facilitator**
  (`gapTopic` 経由で expand/converge/止揚) と **persona** (`prompt-builder` の議題ブロック直後)
  の両 prompt に直近 5 件を注入する。 議題そのものは変えず進め方/トーンのみ steer。 `author_id`
  は監査用に保存するが prompt には出さない (匿名 workspace 方針)。
- **ローカル LLM backend (2026-06-09, 将来 Gemma 等)**: `llm.backend=local` で OpenAI 互換
  `/v1/chat/completions` (Ollama/vLLM/LM Studio/llama.cpp server) に繋ぐ `LocalOpenAiClient`。
  設定は `llm.local{ baseUrl, model, apiKey?, timeoutMs }`(env `LLM_LOCAL_*`、 既定 Ollama
  `http://localhost:11434/v1` + `gemma4:12b`)。persona/classifier/summarizer の全 dispatch が
  `backend=local`(classifier は `classifier.backend=local`)で切替可能。FT は Claude API/CLI に
  無いのでローカル運用はこの backend 経由。疎通確認は `npm run llm:smoke`。既存 Claude 経路は無改変。

- **軽量 Web チャット議論 (2026-06-09, `spec/feature/web-chat.md`)**: Discord 非依存で
  ブラウザの 1 ページチャット (`GET /chat`, loopback) から議論に参加できる。 `scene=web:<room>`
  という Discord と並ぶトランスポートを導入し、 既存の議論エンジン (分類器 → designGap →
  persona-engine → facilitator) をそのまま再利用する。 `event-bridge` の scene 判定を
  `isBoundScene()` (= `discord:` または `web:`) に一般化し、 web room でも「人間優先即応」
  「収束まとめ」まで回る。 出力は Discord に投稿せず core に永続し、 ブラウザが
  `GET /api/chat/:room/messages?since=` でポーリング取得する。 個人データは保存しない
  (author は表示名のみ)。 実装は `src/web-chat/` + `src/api/web-chat-routes.ts`。

## フォーラム集約 (2026-06-06, `discord.forum`)

議論を Discord **フォーラムチャンネル** に集約する (`docs/forum-aggregation.md`)。フォーラムを
「議論カテゴリ」として使い、guild 内の **全 Forum チャンネル** を監視する。

**Discord の議論エンジンは新フロー (`src/flow/`, T1-T7) に全面集約済み (2026-06-13)**。フォーラム
スレッドは旧 auto-discussion ではなく `dispatchFlow` (議論/改善/学習/壁打ち) で起動する。
配線は `gateway.ts` の `ThreadCreate` → `flow/entry-discord.parseForumEntry` → `flow/discord-live.startForumFlow`。
発話は `onUtterance` を `poster.ts` の webhook 投稿に繋いで persona 名でスレッドに流し、結論は
bot 名義で締める (収束時 `finalizeForumPost` で lock+archive+まとめ転記)。

- **入口**: フォーラム新規ポスト (`ThreadCreate`, 親=`GuildForum`) の **starter** で議論を起こす。
  scene = `discord:<guild>/<threadId>`。議題はスレッド名 (無ければ starter 本文)。
- **議論タイプ (必須) は適用タグから取得**: `議論`/`改善`/`学習`/`壁打ち` のいずれかを `parseFlowKind`
  で解決して `dispatchFlow` の FlowKind にする。**タグが無い投稿には議論タイプ選択メニュー
  (StringSelectMenu `flow-pick:<threadId>`) をスレッドに出す**。選択でフローを起動する。
- **観点タグ (任意・複数)**: `機密`/`内部`/`運用`/`開発` (= `FlowTag`)。外部収集可否・ペーパー補足を決める。
- **定義済みフォーラムタグの自動用意**: ClientReady で議論フォーラム (`discord.forum.discussionForumName`,
  既定「議論」) を ensure し、上記 8 タグ (議論タイプ 4 + 観点 4) を `availableTags` に補充する
  (無ければフォーラム自体を作成、`forum-flow-tags.ensureDiscussionForum`)。Manage Channels 権限が要る。
- **壁打ち (sparring) のみ対話継続**: session を threadId で保持 (`discord-live`)、スレッドへの後続投稿を
  `SparringSession.submitUser` に橋渡しする。議論/改善/学習は完走型で後続投稿を取り込まない。
- **クローズ**: 結論到達 (or 壁打ち終了) で `finalizeForumPost` がスレッドを **lock + archive** し、
  まとめを「まとめ投稿」へ転記する。
- **自動作成チャンネル** (ClientReady, guild ごと、Manage Channels 権限が必要):
  - **データ学習依頼** — 貼られた URL を crawl に回す入口 (id は runtime の crawl 集合へ追加)。
  - **まとめ投稿** — 収束まとめの集約先。
- 新フロー実行依存 (`flowLive`: llm/openCore/sentimentClients) が無い (LLM backend 無し) 時は
  フォーラム起動を skip する。`discussionChannelIds` (平文議論) は旧 auto-discussion のまま後方互換で残す
  (議論の正路はフォーラム)。無効化は `discord.forum.enabled=false` (env `DISCUTERE_DISCORD_FORUM_ENABLED`)。

## 学習ビュー × 新フロー (結論統合 / md エクスポート / 議論前自動クロール)

- **新フロー結論を学習ビューに統合**: 「結論」タブは旧 `design_gaps` だけでなく新フローの
  `flow_conclusion` も読む。`src/visualize/flow-conclusions.ts` が `flow_conclusion`+`discussion_paper`+
  `flow_utterance`+`vote` を旧 `conclusions.ts` と同形 (`ConclusionSummary`/`ConclusionDetail`) で返し、
  `/learning/conclusions` が両者をマージ (新しい順)。一意キーは旧=`design_gap.id` / 新=`flow:<sessionId>`、
  `/learning/conclusion?gap=` は prefix でルートする。一覧に `[新フロー]`/`[旧]` バッジ。
- **結論にディスカッションペーパーを併載**: 結論詳細 (`/learning/conclusion` / md エクスポート) に
  議論開始時にペルソナへ配布した**ディスカッションペーパー (議題ブリーフ)** = 観点タグ / 観点補足 /
  ゲームのメカニクス を載せる。新フロー結論のみ (`flow_conclusion` → `discussion_paper` を JOIN で
  `tags_json`/`mechanics_json`/`supplement` も引く)。`ConclusionDetail.paper` (新フロー=値 / 旧
  design_gap=null) に持たせ、UI の論述データ詳細・md (`## ディスカッションペーパー` 節) の両方に出す。
- **結論一覧の SQLite キャッシュ (高速化)**: `/learning/conclusions` (まとめ一覧) は従来
  毎リクエストで KG (481MB) を開き行ごとにサブクエリを撃って重かった。`conclusion_cache`
  テーブル (discutere.db, `src/visualize/conclusion-cache.ts`) に結論サマリ + 話題のサイズを
  焼き、一覧はこれだけ読む (KG 非タッチ)。**論述データ詳細 / md エクスポートは従来通り
  グラフ (KG) を引く** (重い遍歴は閲覧時オンデマンドのまま)。話題のサイズは 2 軸:
  `discussion_volume` (発話数。議論収束ごとに `flow/conclusion.ts` が write-through 更新) と
  `material_count` (話題語を含む外部クロール材料の件数 = KG 直 COUNT の重い計算なので
  `npm run build:conclusion-cache` で別途算出、未算出は -1)。一覧経路は
  `ensureConclusionCacheFresh` で新フロー結論を KG 非依存で追いつかせる (自己修復)。旧
  `design_gaps` 結論 + materialCount のフルバックフィルは build スクリプトで行う。
- **議論の md エクスポート**: `src/visualize/conclusion-markdown.ts` が `ConclusionDetail` を 1 本の md
  (frontmatter + 結論/ディスカッションペーパー/止揚/高評価/議論ログ) にする純関数。UI の「md エクスポート」DL
  (`GET /learning/conclusion/export?gap=`) と CLI `npm run export:discussions`
  (`scripts/export-discussion.ts`、ディスクへ個別書き出し) が同じレンダラを使う。
- **議論前の自動学習クロール (事前学習の UI 化)**: 議論/改善の開始時にテーマの学習データ
  (= 議論と同じ `listExternalVoices` の件数) が `flow.autoCrawl.minVoices` 未満なら、指定ソースで
  クロール → KG 取込してから議論を始める (`src/flow/learning-autocrawl.ts` の `ensureLearningData`)。
  ソースは UI (`/flow` の「学習データ自動取得」欄) で議論ごとに上書き、既定は `flow.autoCrawl`
  (既定 niconico=テーマ検索・キー不要)。対応 source = niconico / youtube / steam(appId) / website(URL)。
  collector は DI 境界 (`DEFAULT_COLLECTORS`) でテスト可能。クロール失敗は議論を止めない (graceful)。
  入口は Web UI (`flow/web/routes.ts`) と Discord フォーラム (`flow/discord-live.ts`、config 既定ソース)。

- **議論前の情報ゲート (情報密度評価フェーズ, `docs/information-gate.md`)**: 旧 autoCrawl の単純カウント
  閾値に代わり、議論/改善フロー開始の直前に **LLM が情報密度 (sparse/moderate/rich) と不足観点 (gaps) を
  評価** し、不足なら不足観点を狙って学習 (クロール) → 再評価する自己改善ループを挟む。**自動モード固定**
  (不足なら自動学習 → 十分になり次第そのまま議論を開始)。LLM 障害時は件数ベース fallback で degrade し
  議論を止めない。設定 `flow.informationGating` (`enabled`/`minDensity`/`maxLearnIterations`/
  `maxGapsPerIteration`、既定 ON・moderate・2・2)。`enabled=false` で旧 autoCrawl にフォールバック。
  クロールのソース/maxItems は `flow.autoCrawl` を流用。本体 `src/flow/information-gate.ts` (DB 非依存・
  単体テスト可) + グルー `information-gate-runner.ts` + 配線 `external-voices.ts`
  (`listExternalVoices` を web/discord 経路に配線した T7 follow-up)。

- **ディスカッションペーパー レビューゲート (`docs/paper-review-gate.md`)**: 議論/改善の開始 **直前**に、
  ペーパー (議題ブリーフ=議題/観点/メカニクス) と集めた情報を **人間が確認 → 自然文で調整 → 承認**
  してから議論を始めるゲート。設定 `flow.paperReview.enabled` (既定 **false** = opt-in)。共有コア
  `src/flow/paper-review.ts` (`buildPaperDraft`/`applyPaperEdit`/`coercePaperDraft`/`renderPaperReview`/
  `isApprovalText`) を Discord/Web 両トランスポートが使う。確定ペーパーは `runFlow(paperOverride)` に渡り
  investigate (step 1) を省略してそのメカニクス/観点補足で議論する (議題/タグは通常引数)。Discord は
  スレッド返信で調整 (例「メカニクスにガチャを追加」)+「開始」返信 or ✅ リアクションで承認
  (`discord-live` の `paperReviewByThread` / `handlePaperReviewReply` / `handlePaperReviewApproval`、
  gateway で壁打ちより優先ルート + `MessageReactionAdd` の ✅ 検知)。Web は
  `/api/flow/:session/paper`(取得ポーリング)+`/paper/edit`(NL 調整)+`/paper/approve`(承認・body の
  直接編集ペーパー可) で確認フォーム調整。`flow.paperReview.timeoutMs`>0 で無操作の自動開始
  (既定 0=無期限・調整で延長、Discord/Web ともサーバ側タイマー)。無効時は従来どおり情報ゲート後に自動開始。

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
