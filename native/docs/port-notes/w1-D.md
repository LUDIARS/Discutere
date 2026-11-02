# port-notes w1-D: クローラの取得元・Discord REST の土台・Slack

job `cpp-node-20261030`。担当 TS 36 ファイルを C++ に移した (Node に残したものは無い)。
加えて TS に対応ファイルの無い `native/src/discord_hook/discord_api.{hpp,cpp}` (Discord REST API v10 クライアント) を新設した。

## 1. 機能の対応表 (TS → C++)

| TS | C++ | 備考 |
|---|---|---|
| src/crawler/types.ts | native/src/crawler/types.{hpp,cpp} | GameKG 等の struct + to_json |
| src/crawler/md-format.ts | native/src/crawler/md_format.{hpp,cpp}, md_format_write.cpp | 読みは `lib::front_matter::parse`。書き (`stringify_game_kg`) は `lib::front_matter::stringify` (w1-C) を呼ぶので別翻訳単位 |
| src/crawler/sources/types.ts | native/src/crawler/sources/types.{hpp,cpp} | ExternalUtterance + to_json/from_json、`FetchFn` (fetchImpl の差し替え口)、crawler の同時実行上限 |
| src/crawler/sources/ingested-store.ts | crawler/sources/ingested_store.* | 同じ DDL |
| src/crawler/sources/raw-store.ts | crawler/sources/raw_store.* | 同じ DDL・UPSERT |
| src/crawler/sources/niconico.ts | crawler/sources/niconico.* | 取得元 URL は `NicoEndpoints` で差し替え可 |
| src/crawler/sources/persona.ts | crawler/sources/persona.* | sha256 先頭 6 hex の「論者#」 |
| src/crawler/sources/reddit.ts | crawler/sources/reddit.* | `RedditEndpoints` |
| src/crawler/sources/steam.ts | crawler/sources/steam.* | `base` 差し替え可 |
| src/crawler/sources/website.ts | crawler/sources/website.* | 本文は `str::strip_html`、タグ領域の切り出しは線形の走査 |
| src/crawler/sources/youtube-comments.ts | crawler/sources/youtube_comments.* | `apiBase` |
| src/crawler/sources/youtube-quota.ts | crawler/sources/youtube_quota.* | mutex 付き |
| src/crawler/sources/youtube-videos.ts | crawler/sources/youtube_videos.* | `apiBase` |
| src/design-diagnostic/contracts.ts | native/src/design_diagnostic/contracts.* | DiagnosticError(code) |
| src/design-diagnostic/prompt-contract.ts | design_diagnostic/prompt_contract.* | 文字列は Node と一致 (golden) |
| src/design-diagnostic/validation.ts | design_diagnostic/validation.* | 長さは UTF-16 単位 |
| src/level-check/contracts.ts | native/src/level_check/contracts.* | |
| src/mechanics-check/contracts.ts | native/src/mechanics_check/contracts.hpp | ヘッダのみ |
| src/discord-hook/types.ts | native/src/discord_hook/types.* | |
| src/discord-hook/normalize.ts | discord_hook/normalize.* | |
| src/discord-hook/diagnostic-reply.ts | discord_hook/diagnostic_reply.* | 返信は `DiagnosticReply{content, file}` (+ `body()`) |
| src/discord-hook/facilitator-directives.ts | discord_hook/facilitator_directives.* | `sql::Database&` を受ける |
| src/discord-hook/forum-flow-tags.ts | discord_hook/forum_flow_tags.* | builder → component JSON (toJSON と同形、golden) |
| src/discord-hook/game-feedback-channel.ts | discord_hook/game_feedback_channel.* | |
| src/discord-hook/managed-channels.ts | discord_hook/managed_channels.* | |
| src/discord-hook/managed-forum-scope.ts | discord_hook/managed_forum_scope.* | `ForumChannelRef` (id/guild/parent/type/isThread/parent) |
| src/discord-hook/paper-gap-ui.ts | discord_hook/paper_gap_ui.* | modal は JSON |
| src/discord-hook/paper-review-ack.ts | discord_hook/paper_review_ack.* | `AckableMessage` + `ackable_message(api, msg)` |
| src/discord-hook/poster.ts | discord_hook/poster.* | すべて DiscordApi 経由 |
| src/discord-hook/reactions.ts | discord_hook/reactions.* | |
| src/discord-hook/system-channel.ts | discord_hook/system_channel.* | |
| (新規) | discord_hook/discord_api.* | Discord REST v10 クライアント + API の struct/JSON |
| src/flow/project-premise.ts | native/src/flow/project_premise.* | |
| src/flow/user-voices/voluptas-impressions.ts | native/src/flow/user_voices/voluptas_impressions.* | |
| src/slack/slack-entry.ts | native/src/slack/slack_entry.* | |
| src/slack/socket-mode.ts | slack/socket_mode.* | `ws` → `di::ws::Client` |
| src/slack/web-api.ts | slack/web_api.* | `WebApiOptions` (base_url 差し替え可) |

### Discord REST クライアントの設計 (discord.js の @discordjs/rest の置き換え)

- `DiscordApi(DiscordApiOptions)`: base URL `https://discord.com/api/v10`、`Authorization: Bot <token>`、
  User-Agent `DiscordBot (https://github.com/LUDIARS/Discutere, 0.1)`。`request(method, path, body, RequestOptions)` が汎用口。
  `RequestOptions{query, reason (X-Audit-Log-Reason を URL エンコード), auth (webhook / interaction は false), files (multipart: payload_json + files[n] + attachments)}`。
- よく使う API: get_current_user (保持)、get_guild / get_guild_channels / get_channel / create_guild_channel / modify_channel、
  get_guild_member / current_member_permissions (owner・Administrator は All、@everyone + role の OR)、
  get/create/edit message、create_reaction、get/create webhook、execute_webhook (thread_id・wait)、
  interaction の callback / @original 編集 / followup、get_gateway_bot、bulk_overwrite_commands。後続の Gateway 担当はこれを使う。
- struct: User / ForumTag / ThreadMetadata / Channel / Role / Guild / Member / MessageReference / Attachment / Message / Webhook / GatewayBot。
  JSON の形は Discord API のまま (snake_case)。必須の id が無い応答は「malformed response」の DiscordApiError。
- レート制限 (discord.js の SequentialHandler の挙動を明示的に書いた):
  - ルート鍵 = メソッド + パス (major parameter = channels/guilds/webhooks の id は残し、他の数値 id は `:id`、
    リアクションの絵文字は `:reaction`、webhook / interaction の token は `:token`)。
  - 同じバケット (応答の `X-RateLimit-Bucket` が分かれば hash + major、分かるまではルート鍵) への要求は mutex で 1 本ずつ送る。
  - `X-RateLimit-Remaining` が 0 で `X-RateLimit-Reset-After` 内なら送る前に待つ。
  - 429: 本文の `retry_after` (秒・小数、無ければ `Retry-After`) と `global` (本文か `X-RateLimit-Global`) を見て、
    global ならプロセス内の全要求を、そうでなければそのバケットを止め、待ってから再試行 (max_retries 回まで)。
  - 待ちが `max_rate_limit_wait` (既定 30 秒) を超えるなら待たずに `DiscordApiError(rate_limited=true)`。
  - 5xx と通信失敗は GET/PUT/PATCH/DELETE だけ 250ms×n で再試行。**POST は再試行しない** (discord.js は POST も再試行していたが、
    メッセージ・webhook 投稿の二重化を避ける)。
  - `shutdown()` で待ちを打ち切る。
- 期限 (要求ごと 15 秒、接続 10 秒)、応答本文 8MiB、JSON 上限 (8MiB / 50 万要素 / 深さ 64)、同時実行 8 本 (クライアントごと)、
  リダイレクトは追わない。
- キャッシュは持たない (discord.js の `guild.channels.cache` 等は REST で取り直す)。自分のユーザーだけ保持する。
  system-channel・poster の webhook は TS と同じくモジュール内で保持する (`clear_*_cache` あり)。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| tests/crawler/external/steam-map.test.ts, reddit.test.ts, youtube-map.test.ts, niconico-map.test.ts, persona.test.ts | crawler.sources |
| tests/crawler/external/website.test.ts | crawler.sources (抽出・正規化・map), crawler.fetch (取得・skip) |
| tests/crawler/external/youtube-fetch.test.ts | crawler.fetch |
| tests/flow/user-voices.test.ts の Voluptas 部分 | crawler.fetch (voluptas impressions), crawler.sources (map) |
| (md-format は TS テスト無し) | crawler.md_format (golden) |
| tests/discord-hook/facilitator-directives.test.ts, reactions.test.ts | discord_hook.stores |
| tests/discord-hook/forum-flow-tags.test.ts, humanize.test.ts, paper-gap-ui.test.ts, paper-review-ack.test.ts, tests/feedback/extract.test.ts | discord_hook.pure |
| tests/discord-hook/managed-forum-scope.test.ts | discord_hook.managed_forum_scope |
| (新規) Discord REST | discord_hook.discord_api, discord_hook.rest_modules |
| tests/slack-entry.test.ts | slack.slack_entry |
| (新規) Slack Web API / Socket Mode | slack.socket_mode |
| (契約・前提) | design_diagnostic.contracts, flow.project_premise |

golden: `native/tests/golden/w1-D-crawler.gen.ts` (fixtures/crawler, slack, flow)、`native/tests/golden/w1-D-discord_hook.gen.ts` (fixtures/discord_hook)。
TS テストのうち担当外のモジュールに依存する部分 (forum-flow-tags.test.ts の `parseFlowKind` / `canCollectExternal`、
slack-entry.test.ts の `parseFlowKind`) は移していない (flow/dispatch・flow/tags の担当分)。
tests/crawler/external/importer.test.ts・summary.test.ts、level-check / mechanics-check の request/tool テストは担当外のモジュールが対象。

## 3. 安全の対応表

| 防御・課金 | 場所 |
|---|---|
| website: 利用者指定 URL は `allow_private_network=false` (解決した IP を検査して固定、リダイレクト先も検査)。既定 fetch で loopback / 169.254 / 10/8 を拒否することをテスト | crawler/sources/website.cpp, crawler.fetch |
| crawler の取得元すべて `allow_private_network=false` (nvComment の server は watch ページ由来の外部値) | niconico/steam/reddit/youtube |
| 応答上限: niconico 検索 4MiB・watch 8MiB・threads 16MiB、Steam 8MiB、Reddit 8MiB (token 64KiB)、YouTube 4〜8MiB、website HTML 4MiB、Voluptas 4MiB、Slack 1MiB、Discord 8MiB。JSON は `parse_json` の limits | 各 .cpp |
| 期限: fetch 既定 30 秒 / Voluptas 10 秒 / Discord 15 秒 / Slack 15 秒 / WS 接続 15 秒 | 同上 |
| 同時実行: crawler の外部取得はプロセス全体 4 本 (`do_fetch` のセマフォ)、Discord はクライアントごと 8 本、+ http::fetch 全体 32 本 | crawler/sources/types.cpp, discord_api.cpp |
| ページ数の上限 (移植で追加): Steam 10,000 / YouTube comments 10,000 / YouTube discover 1,000 | steam.cpp, youtube_*.cpp |
| HTML 抽出は線形 (正規表現なし)。敵対的入力 (`<meta <script ...` の繰り返し 1.3MB) で 10 秒以内をテスト | website.cpp |
| API キー・token をログ・エラー文に出さない: YouTube key (URL に入るのでエラー文に URL を載せない)、Reddit client secret / Bearer、Voluptas Bearer、Discord bot token・webhook token・interaction token (ルートを `:token` に伏せる)、Slack xapp/xoxb。各テストでエラー文・ログに出ないことを確かめる | 各 .cpp, テスト |
| Voluptas: https 必須 (loopback のみ http)、redirect manual (max_redirects=0)、5xx / 接続失敗は VoluptasUnavailable、書き手は匿名固定値 | voluptas_impressions.cpp |
| Di 管理フォーラムの受信境界: guild ID と親 forum ID の組の照合、空 scope は全拒否、未解決の親は読み取り fetch、取得失敗・例外は拒否 (fail closed)。境界テスト (ID 不一致・他 guild・型違い・未取得・fetch 失敗・空 ID・空 forum ID・REST 403) | managed_forum_scope.cpp |
| 外部の声の仮名化 (`論者#xxxxxx`、出所は透過) | persona.cpp |
| Slack Socket Mode: 1 メッセージ 4MiB (WS と JSON)、即 ack、ハンドラは background_executor (満杯なら warn して捨てる)、再接続は 2s→30s の指数バックオフ + 連続失敗 100 回で停止 (移植で追加) | socket_mode.cpp |
| Discord: multipart のファイル名から `"` と改行を除く (ヘッダ注入防止)、リダイレクトを追わない | discord_api.cpp |
| facilitator directive: 本文 300 文字 (UTF-16) 上限、author_id は prompt に出さない、一覧は 1000 行上限で課金 | facilitator_directives.cpp |

## 4. 移していないもの・挙動の差

- **website の本文の改行**: TS は `</p>` 等を改行にしていたが、指示どおり `str::strip_html` (線形) を使うので本文は空白 1 つに詰まる。
  golden テストは「Node の本文を空白で詰めたもの」と一致することを確かめている (タイトル・著者・URL 正規化は完全一致)。
- **website の URL 正規化**: WHATWG URL の全規則は持たない。http/https のみ扱い (それ以外は trim した元の文字列)、
  パスの `.`/`..` の解決・IDNA (非 ASCII ホスト) はしない。非 ASCII の path/query は % エンコードする (golden で一致を確認した範囲)。
- **JS の孤立サロゲート**: `slice` がサロゲートペアの途中で切る場合、JS は孤立サロゲートを残すが C++ は正しい UTF-8 だけを返す
  (facilitator directive の切り詰め・poster の 1900 文字など)。
- **md-format**: front matter の YAML が読めないとき、TS は js-yaml の例外、C++ は `GameKGParseError("frontmatter YAML: ...")`。
  `stringify_game_kg` は w1-C の `front_matter::stringify` を使う。テストは frontmatter オブジェクト (`game_kg_frontmatter`) まで。
  **書き出しの出力は w1-C の merge 後に確認すること**。
- **Discord**: POST は 5xx で再試行しない (上記)。discord.js のキャッシュは使わず REST で取りに行く (system-channel は毎回
  `GET /guilds/{id}/channels` + 権限計算。結果はモジュール内で保持)。権限はチャンネルの permission overwrite を見ない guild 基本権限
  (TS の `me.permissions.has` と同じ範囲)。
- **game-feedback**: `extract_game_feedback(msg, channel, parent, name)` は discord.js の `msg.channel.parent` の代わりに親チャンネルを
  呼び出し側が渡す。channel.parent_id と渡された親の id が違えば拒否する (移植で追加)。createdAt は message.timestamp (ISO) から。
- **niconico**: 検索結果で contentId が文字列でない項目は捨てる (TS は検査せず URL に入れていた)。各 ID は URL エンコードする。
- **steam/youtube**: クエリ値 (filter/order/pageToken/videoId/key) を URL エンコードする (TS は一部そのまま)。
- **Voluptas**: http/https 以外の scheme (`ws://localhost` 等) は拒否 (TS は HTTPS 検査を通して fetch で失敗していた)。
- **facilitator directive の一覧**: 同じ created_at の行は rowid で並べる (TS は SQLite 任意順)。
- **Slack Socket Mode**: 連続失敗 100 回で再接続を止める (TS は無限)。ハンドラは背景プールで実行する。

## 5. 土台に欲しいもの・人間の判断が要ること

- **`ws::Client::close` が冪等でない**: 2 回呼ぶ (例: 相手の disconnect 処理と stop が重なる) と Beast の `soft_mutex` の assert で落ちる。
  socket_mode では 1 回だけ呼ぶよう守ったが、土台側で close を冪等にしてほしい (Gateway 担当も同じ罠に当たる)。
- `str::strip_html` に「ブロック要素の終わりを改行にする」版があると website の本文の段落を保てる (今は空白に詰まる)。
- WHATWG URL のシリアライズ (`new URL(u).toString()`) 相当が base にあると normalize_url の差を無くせる。
- `http::Headers` の多値・`Retry-After` の HTTP-date 形式は読まない (Discord は秒数のみ返すので実害なし)。
- Discord の「無効な要求 (401/403/429) が 10 分 1 万件で Cloudflare に一時 ban」の数え上げは入れていない。必要なら Gateway 担当と決めたい。
