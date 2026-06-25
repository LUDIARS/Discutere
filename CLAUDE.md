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
  (`spec/feature/crawler/EXTERNAL-SOURCES.md` §12)。
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

- **複数ソース横断クロール (2026-06-23)**: 情報ゲートのクロールを単一ソースから **複数ソース横断**に強化。
  `flow.autoCrawl.sources` (既定 `["niconico"]`) を `runInformationGate(crawlSources[])` に渡し、各不足観点を
  全ソースで集める。**`DISCUTERE_YOUTUBE_API_KEY` があれば youtube を自動追加**(キー設定だけで YouTube
  学習が有効化)。website/steam は URL/appId がテーマだけからは決まらないため自動経路では除外 (UI/Discord
  の明示指定時のみ)。`maxItems` 既定 200→300。後方互換: `sources` 未指定なら単数 `source` を流用、
  `crawlSources` 未指定の `runInformationGate` は `crawlSource` にフォールバック。

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

- **ペーパー md 正本化 + Web Notion 風編集 (正規フロー化, 2026-06-25, `docs/paper-review-gate.md`)**:
  ディスカッションペーパーの**本文 (議題/観点補足/メカニクス) を markdown 正本**にする
  ハイブリッド源泉モデル。各 LLM は `buildPaperSystem` が本文 md (`discussion_paper.body_md`) を
  **そのまま system に載せる** (= md を直接参照、未指定の旧経路は構造化から従来組み立て=バイト等価)。
  `src/flow/paper-markdown.ts` (構造化⇄md) + `paper-blocks.ts` (md⇄ブロック) + `paper-revisions.ts`
  (版履歴 `discussion_paper_revision`・「戻す」は前進積み直し) を新設。Web `/flow` の編集ゲートを
  **Notion 風ブロックエディタ**に刷新: ブロック単位で **LLMレビュー (old/new diff→採用/却下)** /
  **根拠クロール (RAG `gatherEvidence`・KG 書込みなし)** / 手編集 / 削除、**「↶戻す」**、
  **「確定」チェックで「議論開始」活性化**。新エンドポイント `/paper/block/review`・`/block/apply`・
  `/crawl`・`/revert` (+既存 `/paper`・`/edit`・`/approve` は md 対応)。設定 `flow.paperReview.webCanonical`
  (既定 **true**) で Web は `enabled` に依らず常にゲート経由の正規フロー (Discord は `enabled` に従う)。
  観点タグは本文 md 外の操作フラグとして構造化維持。確定は `runFlow(paperOverride{bodyMd})` 経由で
  Discord と同一エンジン。→ **Di 内 UI だけで Discord と同じ議論**が回る。

- **チャット議論の入口統一 + 議論ライブ表示 (ペーパーが更新されていく, 2026-06-25)**: トップ (`/`) の
  **「チャット議論」カードを `/flow`** に向け(議論タイプ=議論を既定選択)、`/chat`(軽量チャット)は
  別カード「フリーチャット」に分離。`/flow` で **テーマ + ターン数 + タグ** 記入 → ペーパー編集ゲート →
  確定で **議論が自動進行**。確定後の `/flow` は **2 ペイン**(左=ディスカッションペーパーの md 描画 /
  右=各 LLM の意見の逐次描画)になり、**ペーパーが議論進行で更新されていく**。実装: director が
  ラウンドごとに `renderProgressMarkdown`(base ブリーフ + 議論の経過=まとめ/止揚 + 結論)を焼き
  `updatePaperBody` で `discussion_paper.body_md` を上書き(LLM の system は base のまま=キャッシュ安定、
  表示/永続の body_md だけ育つ・`# 議論の経過` は `stripProgress` で冪等)。status に `paperMd` を追加、
  page.ts は軽量 md レンダラ + 変化フラッシュで描画。`getPaperBodyBySession`/`updatePaperBody` 追加。

- **議論一覧ホーム + 開始遷移 + 開始時保存 (2026-06-25)**: `/flow` を **議論一覧がホーム**に。
  ① 開始送信後すぐ**ペーパー編集画面へ遷移**(草案生成中は「準備中…」表示・フォームは隠す)。
  ② **議論は確定(approve)時点で保存**=`runFlow` 冒頭 `persistPaper` で `discussion_paper` 行が立ち、
  発話は毎ターン `flow_utterance` に永続(収束前でも在庫として残る)。③ **議論一覧**
  (`GET /api/flow/sessions`=開始済み discussion_paper を新しい順・進行中/収束済み両方)から
  「＋新規議論開始」で入力フォーム、項目クリックで既存議論のライブ表示(`openSession`)、各画面に
  「← 議論一覧へ」。実装は `flow/web/routes.ts`(sessions 一覧 API)+ `page.ts`(#list/#backBar/遷移)。

- **ドラフトも議論一覧に出す + status ライフサイクル (2026-06-25)**: `discussion_paper.status`
  (migration `flow_0013`、'draft'|'started') を追加し、ペーパー 1 行が **下書き→進行中→結論あり** と
  状態遷移する設計に。編集ゲートで草案 ready 時に `persistDraftPaper`(status='draft')で永続=
  **議論一覧に「下書き」バッジで出る/クリックで編集再開**(メモリに無くても `getDraftPaper`+
  `rehydrateDraftEntry` で復元)。確定 (approve→`persistPaper`) は同 session 行を status='started' に
  **upsert**(重複行を作らない)。編集(ブロック/NL/戻す)ごとに draft 行を同期。`GET /api/flow/sessions`
  は `state`(draft/live/concluded)を返し、page.ts がバッジ + クリック分岐(`openDraft`/`openSession`)。

- **議論一覧 state フィルタ + ページング (2026-06-26)**: `GET /api/flow/sessions` を「直近100件固定」から
  **絞り込み + ページング**に拡張。`?state=draft|live|concluded`(既定 all)+ `?limit=`(1..200・既定100)+
  `?offset=`。`listFlowSessions({state,limit,offset})`(`discussion-paper.ts`)が correlated subquery で
  結論有無を見て state 絞り込み + 同条件の `total` を返し、レスポンスに `total/limit/offset/hasMore` を追加。
  `page.ts` にフィルタタブ(すべて/下書き/進行中/結論あり)+「もっと見る」(offset 加算追記・1ページ50件)。

- **議論後ペーパー本文の LLM リファイン (opt-in, 2026-06-26)**: 議論終了後に base ブリーフ(議題/観点補足/
  メカニクス)を議論成果(まとめ/止揚/結論)で書き換える。`flow.paperRefine.enabled`(既定 false)。
  **議論ループの後**に走るためペルソナ system(`buildPaperSystem`)はループ中ずっと base のまま固定=
  プロンプトキャッシュ安定を壊さない。`src/flow/paper-refine.ts` の `refinePaperBrief` が改訂 md を返し
  (`# 議題` 欠落/空/LLM失敗は null で degrade)、`director` が結論生成後に refine → `updatePaperDerived`
  で構造化列(観点補足/メカニクス)を追従 → 最終 body は refined base + 議論の経過 + 結論で焼き直す。

- **Discord ペーパー編集パリティ (2026-06-26)**: Web 専用だった議論一覧/下書き再開/版履歴を Discord にも展開。
  ① **`/discutere-discussions [state]`** = 議論一覧 slash(下書き/進行中/結論あり・`listFlowSessions` 再利用・
  ephemeral)。② Discord のペーパーレビューも **`persistDraftPaper`(status='draft', session_id=threadId)で永続**
  → 議論一覧に「下書き」で出る + **再起動跨ぎで再開**(メモリに無ければ `getDraftPaper`+`rehydratePaperReview`
  で復元、guildId は返信/リアクション文脈から補完)。承認は `runDiscussionDispatch` が **sessionId=threadId** で
  `persistPaper` upsert(draft→started、bodyMd も override で運ぶ)。③ **「戻す」返信**で版履歴 revert
  (`isRevertText`+`revertLast`、編集ごとに `appendRevision`)。Notion 風ブロック編集 UI 自体は Web ネイティブ
  操作なので Discord には載せない(返信ベースの NL 調整 + 戻す + ✅/「開始」承認で機能パリティ)。

- **ペーパーの分量増強 (感想3倍 + メカニクスLLM増補, 2026-06-23)**: ペーパーが薄い問題への対処。
  設定 `flow.paperRichness` (`voices` 既定15 / `mechanicsTarget` 既定30 / `enrichMechanics` 既定true /
  `enrichModel`)。**感想**はペルソナ/ペーパーに載せる件数を 5→`voices` に増やす (director の voiceCache
  lookup + `buildUserOpinionsText`、レビュー表示サンプルは 3→9)。**メカニクス**は供給源 (data/games の
  md が 1 ゲーム ~10 件) が目標に満たないため、`src/flow/mechanic-extract.ts` の `enrichMechanics` が
  集めた感想を根拠に LLM で追加抽出して `mechanicsTarget` 件まで増やす (name で重複除去 + クランプ)。
  配線: paper-review 経路 (`buildPaperDraft`) と非レビューの `runFlow` 投資ゲート後の両方。**材料 (感想)
  が無ければ増補しない**・LLM 失敗時は既存件数で degrade (議論を止めない)。LLM コスト=議論ごとに 1 回増。

## 個人データ

匿名 workspace (`DISCATIER_WORKSPACE` 既定 `knowledge`)。攻略 KG / 議論ノードに編集者名・アカウント名を保存しない (`spec/feature/crawler/DESIGN.md` 準拠)。

**外部発話の露出方針 (2026-06-08, `spec/feature/crawler/EXTERNAL-SOURCES.md` §6)**: 出所メタ
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

**外部の声検索の取りこぼし修正 (2026-06-23)**: `listRelevantExternalVoices` は元々 (a) 直近 300 件
(CANDIDATE_CAP) しか候補にせず + (b) 議題文をフル 1 語で `includes` 照合していたため、材料が
KG に大量 (41 万件超) あっても新着に埋もれた + 日本語フル議題文が一致せず **常時 0 件**になっていた
(情報ゲートが永遠に sparse 判定で無駄クロール、ペーパーレビューの「集めた情報 0 件」表示の主因)。
修正: `keyword-terms.ts` の `extractKeyTerms` で議題を検索語に分解 (句読点/助詞分割・依存ゼロの素朴
規則) し、関連語があれば **全件を SQL LIKE で照合** (直近 N 件制限を撤廃)、無ければ従来どおり直近 N 件を
opinion-score で拾う。実 DB で 0→数十〜数百件に改善。

**ゲーム名の別名展開 (#301, 2026-06-23)**: 口語略称 (「モンスト」4000 件超 vs 正式名「モンスターストライク」
数十件) を取りこぼさないよう、検索語をゲーム名の別名で拡張する。`game-aliases.ts` が games タイトル
"EN (JP)" を機械分解 (`parseTitleAliases`) し、機械導出できない口語略称の静的辞書 (`STATIC_ALIAS_GROUPS`)
とマージ (`buildAliasGroups`)、`expandAliases` で検索語を双方向 (略称⇄正式名・和名⇄英名) に拡張する。
`listRelevantExternalVoices` (ライブ RAG/ゲート/ペーパー) と `build-conclusion-cache` の materialCount の
両方に配線。実 DB で「モンスターストライク…」32→4211 件。新ゲームは静的辞書に略称を足す。
