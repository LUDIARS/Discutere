# port-notes w4-T: Discord Gateway v10 クライアント (discord.js の WebSocket 部分の置き換え)

job `cpp-node-20261030`。TS に対応ファイルの無い新規モジュール `native/src/discord_hook/gateway_client.{hpp,cpp}` を作った。
後の担当が `src/discord-hook/gateway.ts` (イベントの振り分け・ClientReady 時の処理) をこの上に移す。

## 1. 機能の対応表 (TS / discord.js → C++)

| TS / discord.js | C++ | 備考 |
|---|---|---|
| `new Client({ intents: [...] })` (gateway.ts) | `GatewayClientOptions::intents` (既定 `DEFAULT_GATEWAY_INTENTS`) | Guilds / GuildMessages / MessageContent / DirectMessages / GuildMessageReactions = 38401。`gateway_intent::*` は GatewayIntentBits と同じ値 |
| `client.login(token)` | `GatewayClient::start()` | 専用の制御スレッドを起こす |
| `client.destroy()` | `GatewayClient::stop()` | close(1000) → close_grace だけ待つ → ws を壊す → 制御スレッドを join。backoff・identify 待ち・session_start_limit 待ちの途中でも戻る |
| `client.on(Events.X, ...)` | `on_dispatch(event 名, d)` | event 名は Gateway の `t` (MESSAGE_CREATE / THREAD_CREATE / INTERACTION_CREATE / MESSAGE_REACTION_ADD / READY …)。discord.js の Events 名への読み替えと `d` からの struct 化 (`message_from_json` 等) は gateway.ts の担当が行う |
| `Events.Error` | `log::warn/error` + `on_fatal(code, reason)` | 再接続しない close (4004 等)・`/gateway/bot` の 401・連続失敗の上限で呼ぶ |
| @discordjs/ws の WebSocketShard | `GatewayClient::Impl` (状態機械) | 下記 |
| (なし) | `GatewaySendLimiter` / `GatewayDedupe` / `gateway_dedupe_key` / `classify_close_code` / `build_gateway_url` / `is_allowed_gateway_url` | 状態機械の部品。純粋でテストしやすい形で公開 |

### discord.js に頼っていた挙動を C++ でどう明示的にしたか

- **接続**: `GET /gateway/bot` (w1-D の `DiscordApi::get_gateway_bot`、Gateway 専用の DiscordApi を持つ) で URL と
  `session_start_limit` を得て `<url>/?v=10&encoding=json` に `di::ws::Client` で接続 (圧縮なし、`encoding=json`)。
  discord.js は `/gateway/bot` を login 時だけ取るが、ここでは**新しい session を始める (identify) たびに取り直す**
  (session_start_limit を正しく守るため。resume は取り直さない)。
- **Hello / heartbeat**: `heartbeat_interval` を [min,max] にクランプし、初回は `interval × random_unit()`、以後 interval ごと。
  heartbeat の `d` は最後の seq (無ければ null)。次の heartbeat の時点で前回の ACK (op 11) が来ていなければ
  **ゾンビ接続**として ws を壊し (close frame を送らない = session は resume 可能)、resume で繋ぎ直す。
  サーバからの op 1 には即座に heartbeat を返す (定期の予定は変えない)。Hello が `hello_timeout` 内に来なければ繋ぎ直す。
- **Identify / Resume**: identify は `{token, intents, properties{os, browser, device}}`。resume は
  `{token, session_id, seq}` を READY の `resume_gateway_url` に送る。
- **op 7 Reconnect** → resume。**op 9 Invalid Session**: `d=true` かつ session があれば resume、それ以外は session を捨て
  `invalid_session_wait_min..max` (既定 1〜5 秒) の一様乱数だけ待って identify。
- **close code**: `classify_close_code`。4004 (認証失敗)・4010〜4014 は Fatal (再接続しない・`on_fatal`)。
  1000・4003・4007・4009 は session を捨てて identify。それ以外 (4000 等、通信断 1006、メッセージ上限超過 1009) は resume。
- **再接続**: 接続できない失敗が続く数 n に対し、`min(reconnect_base × 2^n, reconnect_max)` の半分 + 一様 jitter (半分)。
  接続が確立していた (READY/RESUMED を受けた) 後の切断やサーバ指示 (op 7 / op 9) は失敗として数えない。
  `max_consecutive_failures` (既定 0 = 諦めない) を超えたら止まって `on_fatal(0, ...)`。
- **送信のレート制限**: `GatewaySendLimiter` (60 秒 120 件)。heartbeat / identify / resume は 120 件まで、
  それ以外 (外からの `send()`: op 3/4/8 のみ) は 120 − `send_reserved` (既定 5) 件まで。外からの送信は
  上限 `max_send_queue` (64) のキューに積み、窓が空くたびに制御スレッドが送る。1 送信 4096 バイトまで。
  heartbeat 等が万一上限に当たったら warn して送る (送らないとゾンビ扱いになるため)。
- **identify の制限**: `session_start_limit.remaining == 0` なら `reset_after` (上限 `max_session_start_wait`) だけ待ってから
  `/gateway/bot` を取り直す。`max_concurrency` は `identify_window` (5 秒) あたり max_concurrency 回まで (単一 shard)。
- **重複排除** (discord.js は resume の再送を素直に emit するが、Discutere の議論起動・承認は 2 度起きると困る):
  1. 同じ session 内で `s` が直前の seq 以下の dispatch は捨てる (`stale_seq_dropped`)。
  2. `GatewayDedupe` (既定 10 分・1 万件) で MESSAGE_CREATE / INTERACTION_CREATE / THREAD_CREATE は `d.id`、
     MESSAGE_REACTION_ADD は `message_id:user_id:emoji` を覚え、2 度渡さない (`duplicates_dropped`)。
     MESSAGE_REACTION_REMOVE は同じ鍵を忘れるので、付け直した ✅ は新しいイベントとして渡る。MESSAGE_UPDATE 等は対象外。
- **キャッシュ**: discord.js の guild / channel / message キャッシュと partials (Channel / Message / Reaction) は持たない。
  dispatch の `d` をそのまま渡し、足りない情報は gateway.ts 側が REST (`DiscordApi`) で取る (w1-D の方針と同じ)。
  GUILD_CREATE 等も on_dispatch に渡すので、必要ならそこで ID の集合だけ保持する。
- **ハンドラの実行**: 制御スレッドは受信を読んで状態を進めるだけで、`on_dispatch` は既定で `background_executor()`
  (`executor` で差し替え可) に渡す。満杯なら**捨てて warn** (`dropped_queue_full` を数える)。Discord は再送しないので
  そのイベントは失われる (discord.js でも処理中の例外で同じく失われていた)。executor に渡すため**イベント間の順序は保証しない**
  (discord.js の async ハンドラも await で交互に進んでいた)。

## 2. テストの対応表

TS に対応するテストは無い (discord.js 任せだった)。新規に `native/tests/discord_hook/gateway_client_tests.cpp` (ctest `discord_hook.gateway_client`)。
テスト内に Beast の偽 Gateway (非同期 WebSocket サーバ、接続ごとにテスト側から送受信・close) と偽 REST (`GET /gateway/bot`) を立てる。

結果: `discord_hook.gateway_client` 16/16 (約 4 秒、連続 20 回・4 並列 ×3 回でも失敗なし)。ビルドディレクトリの ctest 全体 130/130 通過。

| 観点 | TEST_CASE |
|---|---|
| close code の分類・URL の組み立てと許可・intents の値 | close codes, urls and intents |
| 送信の窓 (上限ちょうど・+1・reserved) | send limiter keeps reserved slots for heartbeats |
| 既読集合 (ttl・容量・forget・鍵) | dedupe set: ttl, capacity and keys |
| Hello → identify (token・intents・properties)・heartbeat/ACK・op 1 への即応・dispatch・stop で 1000 | hello, identify, heartbeat/ack, server heartbeat request, dispatch and stop |
| ACK 欠落 → ゾンビ検出 → resume (session_id・seq)・token をログに出さない | missing heartbeat ACK is a zombie connection ... |
| op 7 → resume、op 9 true → resume、op 9 false → identify | op 7 reconnect and op 9 invalid session ... |
| 4004 で止まる (再接続しない)・`/gateway/bot` の 401 も fatal | close 4004 stops without reconnecting ... |
| 4000 → resume、4009 → identify | close 4000 resumes; close 4009 identifies again |
| 重複排除 (seq の戻り・id・reaction の付け直し・resume 後の再送) | duplicate events are delivered once ... |
| 送信レート制限 (窓あたりの件数・全件が後で届く・op 制限・4096 バイト超) | outgoing sends respect the rate limit ... |
| 1 メッセージの上限ちょうど / +1 (+1 は切れて resume) | message size limit ... |
| 壊れた JSON・object でない・深さ超過・空フレームは捨てて接続を保つ | broken JSON frames are dropped ... |
| Hello が来ない → 繋ぎ直す | no Hello within the deadline reconnects |
| session_start_limit (remaining=0 → reset_after 待ち)・max_concurrency | session_start_limit and max_concurrency delay identify |
| executor 満杯で捨てて warn | handlers run on the executor; a full queue drops ... |
| backoff 中でも stop がすぐ戻る・2 度目の stop | stop returns promptly during backoff |

## 3. 安全の対応表

| 防御 / 入力経路 | C++ |
|---|---|
| bot token をログ・エラー文に出さない | identify / resume の本文はログしない。ログは op・close code・件数・バイト数だけ。REST のエラーは DiscordApiError (token を含まない)。テストでログに token が出ないことを確かめる |
| Gateway の WS フレーム | `max_message_bytes` (既定 8MiB): ws の `read_message_max` + lorica::input_budget (infra/ws_client)。超過は 1009/1006 で切れ resume |
| フレームの JSON | `parse_json(text, json_limits)` (既定 8MiB / 50 万要素 / 深さ 64)。壊れたもの・object でないものは捨てて warn (`malformed_frames`)、本文はログしない |
| Gateway の URL (`/gateway/bot` の url・READY の resume_gateway_url) | `is_allowed_gateway_url`: 本番は `wss://` かつ host が discord.gg / discord.com かその配下だけ。それ以外の resume URL は捨てて identify し直す。ws は公開アドレスだけに繋ぐ (allow_private_network=false) |
| heartbeat_interval | [min_heartbeat_interval, max_heartbeat_interval] にクランプ。無い・0 以下は接続をやり直す |
| 外からの送信 | op は 3/4/8 だけ (identify・resume・heartbeat を偽れない)、1 件 4096 バイト、キュー 64 件、レート制限の内側 |
| 外部 I/O の期限 | 接続 `connect_timeout`、Hello `hello_timeout`、ACK は heartbeat 1 周期、REST は DiscordApi の期限、stop の close 待ち `close_grace` |
| ハンドラの同時実行 | executor (上限つきキュー) に渡す。満杯は捨てて warn |
| 状態の共有 | 状態機械は制御スレッド 1 本。ws の I/O スレッドは受信を mutex 付きのキューに積むだけ。ws::Client の破棄は制御スレッドで行う (I/O スレッドの中で壊さない) |

## 4. 移していないもの・挙動の差

- **gateway.ts 本体** (イベントの振り分け・ClientReady の処理) は担当外 (後の担当)。
- **シャーディング**: 単一 shard (identify に `shard` を付けない)。`/gateway/bot` の `shards` が 2 以上でも 1 本で繋ぐ
  (Discutere は数 guild 規模。2500 guild を超えると 4011 で Fatal になる)。discord.js も shardCount 未指定なら同じく単一 shard で始める。
- **圧縮** (zlib-stream / payload compression) と ETF は使わない (`encoding=json`、圧縮なし)。zlib は依存の表に無い。
- **キャッシュ・partials**: 持たない (上記)。discord.js の `client.channels.cache` 等に頼っていた箇所は REST で取り直す。
- **discord.js との差**: 
  - resume の再送・再接続での重複を捨てる (discord.js は 2 度 emit しうる)。
  - `/gateway/bot` を identify のたびに取り直す。
  - POST の再試行をしないのは REST (w1-D) の方針で、Gateway には関係しない。
  - ハンドラ間の順序は保証しない (executor に渡すため)。同じスレッドの返信の順序が必要な処理は gateway.ts 側で
    スレッドごとに直列化する (TS の paper-review-session が既にスレッドごとに直列化している)。
  - stop() は executor に渡したハンドラの終わりを待たない (呼び出し側が executor を止める)。
- presence の初期値 (identify の `presence`) は送らない (TS も設定していない)。

## 5. 土台に欲しいもの / 人間の判断が要ること

- `ws::Client` (infra) はメッセージ上限超過を Beast が先に検出すると `on_close(1006, "message too big")`、lorica が検出すると
  `on_close(1009, ...)` と、同じ原因で close code が二通りになりうる。どちらも resume なので Gateway では差が出ないが、
  土台で 1009 にそろえると分かりやすい。
- `ws::Client` の破棄は I/O スレッドを join するので、`on_message` / `on_close` の中で Client を壊せない
  (ここでは制御スレッドへ受け渡して回避した)。土台のヘッダにこの制約を明記してほしい。
- `ws::Client::connect` は同期で `connect_timeout + 1s` まで戻らない。stop() はその間だけ待つ (既定 15 秒)。
  途中で中断できる connect があると stop がより速くなる。
- 人間の判断: 本番の Gateway URL を discord.gg / discord.com 配下に限定した (`/gateway/bot` の応答が想定外の
  host を返したら繋がない)。Discord が別ドメインに移すと繋がらなくなるので、その場合はここを直す。
- 人間の判断: executor 満杯で捨てたイベントは失われる (Discord は再送しない)。議論の起動が落ちる可能性があるので、
  運用で `dropped_queue_full` を監視するか、gateway.ts 担当が重要イベントだけ専用のキューに分けるかを決めてほしい。
