# port-notes w6-main — src/index.ts → native/src/main.cpp (サーバの組み立て) と互換性の検査

job `cpp-node-20261030`。担当: `src/index.ts` (933 行) を `native/src/main.cpp` に移し、起動の結合テストと
Node 版との互換性検査スクリプト (`native/tools/compat-check.ts`) を足した。これで常駐サーバから到達する TS はすべて C++ にある。

## 1. 機能の対応表 (index.ts → main.cpp)

index.ts と同じ順序・同じ条件で組み立てる (Hono の登録順 = `http::Router` の登録順)。

| index.ts | main.cpp |
|---|---|
| `import "./db/connection.js"` (最初に main DB を開く) | `db::get_main_db()` を最初に呼ぶ |
| `installConsoleCapture()` | `observability::install_console_capture()` (di::log の sink) |
| `installVestigium({serviceCode:"discutere", captureConsole:true, pinoTransport:false})` | `log::Vestigium{service_code:"discutere"}` (コンストラクタが log の sink を登録 = captureConsole 相当。logs_dir は env `VESTIGIUM_LOGS_DIR` か `./logs`) |
| `getConfig()` / `assertStartupHealth(config)` | `config::get_config()` / `assert_startup_health(config)` (致命は log::error して終了コード 1) |
| `app.use("*", cors({origin: frontendUrl, credentials:true, allowMethods, allowHeaders}))` | `app->use("*", http::cors(frontendUrl, true, {GET,POST,PUT,DELETE,OPTIONS}, {Content-Type,Authorization,X-User-Id,X-User-Role}))` |
| `app.get("/health")` | 同じ JSON `{"status":"ok","service":"discutere"}` |
| `/` topPageRoutes → `/` createGuideRoutes(config.discord) → `/` learningViewRoutes → `/internal` workerRoutes | 同じ順で `mount` |
| `app.use("/api/*", userContext())` | 同じ位置 (worker callback の後、`/api` の各ルートの前) |
| `/api`: glabReviewTrend → conclusionPublication → admin → dashboard → errors → consensus → queue → noise → datasource → workerPoolControl → tuning → persona → personaBridge → impressionBridge → blackbox → kgMigration | 同じ順で `mount("/api", …)` (consensus は内部で consensus-persona / gap-creation を mount 済み) |
| `startKgSyncScheduler(config)` | `kg_sync::start_kg_sync_scheduler(config)` (停止関数を終了時に呼ぶ) |
| `buildClassifierLlm()` | 同じ分岐 (off / anthropic / local / claude-cli)。anthropic はキーがあれば warn して ClaudeCliClient (§4) |
| persona-engine の IIFE (runtime-settings → setRolePromptResolver → setRuntimeSettings → LLM の選択 → classifier → peDb / core / reactions 表 / adapter / engine / bridge / excludeProviders の rule 無効化 / setPersonaEngine / start / facilitator / consensus scorer / auto seed) | 同じ順の lambda。try の範囲も同じ (runtime-settings と LLM の組み立ては try の外、peDb 以降は try の中で失敗は warn して null) |
| economy-graph routes (`/`) | `api::create_economy_graph_routes({workspace, AnalyzeEconomyDeps{ClaudeCliClient}})` (w3-K の方針: API キーではなく claude -p) |
| web-chat (`setWebChatDeps` + `/`) | `api::set_web_chat_deps` + mount (`classifyInboundMessage` は classifier LLM の starter、表示名は peDb) |
| flowClaudeLlm / flowEngineLlm | worker-pool: `FallbackLlm(WorkerPoolClient, ClaudeCliClient)`、それ以外: persona の LLM か classifier。`ModelRouterLlm(flowClaude, CodexCliClient{cwd: worker-home})` |
| `/flow` (`setFlowWebDeps` + flowRoutes + specExtractRoutes) | `flow::web::set_flow_web_deps` + `flow_routes()` + `spec_extract_routes()` (flowEngineLlm がある時だけ) |
| external discussion (`/internal/external-discussion`) | 同じ条件 (秘密あり・モデル無しは起動失敗、秘密 + モデル + LLM で service、それ以外は 503 を返すルート) |
| `startBackupScheduler(config)` | `backup::start_backup_scheduler(config)` |
| `startCostRelay(getFlowDb(), {intervalMs, resolve})` + 起動ログ | `flow::start_cost_relay` (resolve は runtime-settings を毎 tick 読む)。ログの文言は同じ |
| `buildQueueText()` | `queue::format_queue_text(build_queue_snapshot(core, peDb, ws))` (core は都度 open/close) |
| `GameFeedbackStore(new Database("./data/game-feedback.db"))` | 同じパス |
| `startDiscordGateway({...})` + `.then(handle => …)` | 専用スレッドで `discord_hook::start_discord_gateway(deps)`、戻ったら late-binding を結線 (下表)。起動失敗は warn で続行 |
| Slack (`createSlackRouter` + `startSocketMode`) | `slack::create_slack_router` + `slack::start_socket_mode` (同じ条件: enabled + appToken + botToken + channelIds + LLM) |
| 起動ログ (`Discutere listening on …` 〜 `WorkerPool: …`) | 同じ文言 (TS ソースの文字化け `U+2001 E` も同じバイト列で出す) |
| `gracefulShutdown` (SIGINT / SIGTERM) | 同じ文言 `\n<SIG> received — shutting down (worker pool stop)...`。§4 の順で止めて終了コード 0 |
| `serve({port, hostname: host})` (既定 127.0.0.1:3110) | `http::Server` (host / port は config.server)。listen 失敗は終了コード 1 |

### 結線した late-binding

| 結線 | 先 | 内容 |
|---|---|---|
| `setRolePromptResolver` | persona_prompts | runtime-settings の `get_role_prompt` (弱参照) |
| `setRuntimeSettings` | tuning_routes | ストア本体 |
| `setWorkerPool` / `setWorkerPoolControl` | api/worker・worker_pool_control | backend=worker-pool のときの WorkerPool |
| `setPersonaEngine` / `setFacilitator` | admin_routes | engine / facilitator (facilitator は personaEngine.enabled + facilitator.enabled のときだけ) |
| `setWebChatDeps` / `setFlowWebDeps` | web_chat_routes / flow/web/routes | 依存一式 |
| facilitator `onConverged` → `finalizeForumPost` | gateway | Gateway 起動後かつ `discord.forum.enabled` のときだけ有効 (TS の forumFinalizer)。finalize は background_executor で |
| consensus scorer `react` → `reactToMessage` | gateway | Gateway 起動後に有効 (TS の consensusReact) |
| `setGatewaySweeper` → `sweepUnseeded` | admin_routes | Gateway 起動後 (limit は整数に丸める) |
| `entryParsers` | gateway ← flow/entry_discord | `parse_forum_entry` / `parse_rounds_turns` / `parse_opponents` (w5-U の「main で結線」) |
| `flowLive` | gateway ← flow/discord_live | llm / openCore / listExternalVoices / sentimentClients / workspaceId / getYoutubeApiKey (api は Gateway が自分の REST を入れる) |
| `mechanicsCheck` / `levelCheck` | gateway | `DiagnosticDependencies{flowEngineLlm}` |
| `onReaction` | gateway → discord_hook::apply_reaction | persona の core に重みを加算 (失敗は warn) |
| `onGameFeedback` | gateway → GameFeedbackStore::add | 投稿者は渡さない |
| `crawlDeps` | gateway | createCore / workspace / getYoutubeApiKey / `reddit_credentials_from_env()` / `create_llm_summarizer(persona LLM)` |
| router の `getEngine` / `buildQueueText` / `triggerBackup` / `triggerEconomyAnalysis` / `triggerPersonaGenerate` / `classifyInboundMessage` | gateway の command_router | TS と同じ中身 (経済分析は `analyze_economy_in_background` + `to_slug`、ペルソナ生成は `run_adopt_from_kg({sourceFilter, dry:false})`) |
| adapter の `onPostedUtterance` / `onPostedHypothesis` | discussion_bridge | 背景で `post_discussion_to_discord` → `record_posted_message` (TS は await。C++ の adapter は呼んだスレッドで callback を呼ぶので I/O は背景に) |
| `sentimentClients.main` | flow / slack | LLMClient を `SentimentLlmClient` に包む小さな adapter (TS は構造的型付けで同じ物を渡していた) |

## 2. テストの対応表

index.ts に TS テストは無い。新規:

| 観点 | ctest |
|---|---|
| 子プロセスで起動 → `/health` (本文・Content-Type)・`/`・`/flow`・`/chat` (HTML)・`/api/admin/status` (loopback=admin で 200、X-User-Id だけなら 403)・存在しない経路の 404 `404 Not Found`・CORS preflight (204・Allow-Origin=frontendUrl・credentials・methods・headers)・`/internal/worker/register` (503)・`/internal/external-discussion/consider` (秘密なし 503)・SIGTERM で終了コード 0 と終了ログ・起動ログ (Gateway skip / persona-engine disabled / flow-ui)・終了後は繋がらない | main.main |
| production で bot token 無し → startup guard で終了コード ≠ 0 | main.main |
| external discussion の秘密あり・モデル無し → 起動失敗 (TS の throw と同じ) | main.main |

互換性 (golden の代わりに Node と C++ を並べて実際に比べる): `native/tools/compat-check.ts` (§6)。

## 3. 安全の対応表

| 防御 | main.cpp |
|---|---|
| HTTP bind の既定 loopback (127.0.0.1) | config.server.host をそのまま (BACKEND_HOST で上書き) |
| CORS は frontendUrl のみ + credentials | 同じ (結合テストで確認) |
| `/api/*` の userContext の位置 | worker callback の後、すべての `/api` ルートの前 (結合テストで loopback=admin / ヘッダ偽装なし=403 を確認) |
| `/internal/worker/*` の loopback 限定 | worker_routes 側 (userContext より前に mount するのも TS と同じ) |
| external discussion の default-deny | 秘密が無ければ 503 (結合テスト)、秘密があってモデルが無ければ起動しない |
| startup guard | production で bot token 無しは起動しない (結合テスト) |
| 秘密をログに出さない | 起動ログは設定の有無と URL・モデル名だけ (token / API キー / 共有秘密を出さない)。Gateway・Slack の token は各モジュールの中だけ |
| 本文上限 | 各ルートの `RouteOptions` / `body_limit` を Router がそのまま持つ (HTTP サーバが `max_body_for` で課す) |
| 終了処理の期限 | 後始末全体に 20 秒の watchdog、executor の停止待ちは 5 秒で打ち切る (LLM 待ちで終了が固まらない) |
| シグナルハンドラ | async-signal-safe な `sig_atomic_t` の代入だけ (mutex を触らない) |

## 4. 移していないもの・挙動の差

- **AnthropicSdkClient を持たない** (w1-B / w2-I の方針)。TS が AnthropicSdkClient を作っていた条件 (backend が worker-pool / local /
  claude-cli 以外 = 既定の "anthropic" や "mock" で anthropicApiKey あり) では **warn して ClaudeCliClient (claude -p)** にする。
  キーが無ければ TS と同じく persona-engine は skipped。classifier の backend=anthropic もキーがあれば warn して claude -p、無ければ null (TS と同じ)。
  worker-pool 時の flow LLM の鎖は TS の `worker-pool → SDK(OAuth/cache) → claude -p` から SDK を外して `worker-pool → claude -p`。
  (Claude Code の OAuth トークンを読む `readClaudeCodeToken` も使わない。cache_control による usage 計上は claude -p の usage に依る)
- **backend=mock**: TS の index には mock の LLM は無い (= キーが無ければ persona-engine skipped、/flow も mount されない)。C++ も同じ。
- **経済グラフ**: TS は API キーで分析 (キーが無ければ "ANTHROPIC_API_KEY not set")。C++ は backend に依らず claude -p (w3-K)。
- **Gateway の起動**は専用スレッド (TS の Promise)。`start_discord_gateway` が戻ったら late-binding を結線する。HTTP の listen はこれを待たない。
  TS でも結線は login 完了後なので、起動直後の数百 ms に収束した議論は finalize されない点も同じ。
- **終了処理**: TS は worker pool / kg sync / slack / vestigium を止めて `process.exit(0)`。C++ は加えて HTTP を最初に止め、Gateway・
  cost relay・backup scheduler・facilitator / consensus scorer / auto seed / engine / bridge・global_scheduler・background_executor を止めてから
  Vestigium を閉じ、`std::_Exit(0)` (静的オブジェクトの破棄順に依存しない。TS の process.exit と同じく即終了)。
- **ロガーの meta**: facilitator / consensus / auto-seed のロガーは `  [tag] msg <meta の JSON>` (TS は console.log の util.inspect 書式)。
- **起動ログの順**: TS の `discord-gateway: skipped` は Promise の中なので `Discutere listening …` より前に出ることも後に出ることもある。C++ は専用スレッドなので同様に前後しうる。
- **listen 失敗**: TS は serve の例外 (EADDRINUSE) でプロセスが落ちる。C++ はログを出して終了コード 1。

## 5. 土台に欲しいもの / 人間の判断が要ること

- **土台**: `proc` に「起動して pid を返し、後で待つ / シグナルを送る」非同期版があると、結合テストが Vestigium のログから pid を拾わずに済む
  (今は `proc::run` を別スレッドで走らせ、JSONL の `pid` を読んで SIGTERM を送っている)。
- **土台**: `Executor::stop` に期限つき版 (走っている仕事を待つ上限)。今は main で別スレッドから stop して 5 秒で見切っている。
- **人間の判断**: 起動ログの TS ソースの文字化け (`Gateway (WS)  Eslash`) をそのまま出している。直すなら TS と同時に。
- **人間の判断**: backend=anthropic + API キーの設定を claude -p に読み替えること (warn を出す)。claude CLI が無い環境ではここで LLM が使えなくなる。

## 6. 互換性の検査 (compat-check)

(結果は下に追記)
