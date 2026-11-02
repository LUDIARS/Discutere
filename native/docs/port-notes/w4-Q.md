# port-notes w4-Q: 残りの API ルート・Discord のコマンドルータ・フォーラム監視・発話の投入口

job `cpp-node-20261030`。担当 TS 8 ファイルをすべて C++ に移した (Node に残したものは無い)。

## 1. 機能の対応表 (TS → C++)

| TS | C++ | 備考 |
|---|---|---|
| src/core/projection/message-input.ts | native/src/core/projection/message_input.{hpp,cpp} | `submit_message(SubmitMessageInput)` → `SubmitMessageResult{utteranceId, commandResult?}`。コマンドの振り分けは TS と同じ順・同じ名前 (w3-N の handler を呼ぶ) |
| src/fundamentum-export/index.ts | native/src/fundamentum_export/index.hpp | 再 export のみ (export / list / snapshot / store は w3-L) |
| src/api/blackbox-routes.ts | native/src/api/blackbox_routes.{hpp,cpp} | `blackbox_routes(BlackBoxFactory = flow DB の density blackbox)`、`reset_blackbox_routes()` (TS の `_resetBlackboxRoutes`)。インスタンスと engine 操作は mutex で直列化 |
| src/api/consensus-routes.ts | native/src/api/consensus_routes.{hpp,cpp} + native/web/api/consensus.html | `consensus_routes(CoreOpener)`。w3-M の consensus_persona_routes / gap_creation_routes を先頭に mount (TS の `route("/", ...)`)。`open_active_core()` (active KG を開く既定の opener) を公開 |
| src/api/learning-view-routes.ts | native/src/api/learning_view_routes.{hpp,cpp} + native/web/api/learning.html | `learning_view_routes(LearningViewDeps)` (openCore / flowDb / openAttribution / learningCachePath を注入可能。既定は TS と同じ) |
| src/api/web-chat-routes.ts | native/src/api/web_chat_routes.{hpp,cpp} + native/web/api/chat.html | `web_chat_routes()` + `set_web_chat_deps(WebChatDeps)` (TS の `setWebChatDeps`。mutex で保護) |
| src/discord-hook/command-router.ts | native/src/discord_hook/command_router.{hpp,cpp} | `route_slash_command` / `route_inbound_message` / `route_forum_post`。入口 `handle_slash_interaction(interaction JSON, deps, SlashReplier&)` / `handle_inbound_message_json`。返信 `RestSlashReplier` (DiscordApi の interaction callback type 4) |
| src/discord-hook/forum-monitor.ts | native/src/discord_hook/forum_monitor.{hpp,cpp} | `handle_forum_thread_create(ThreadCreate の d, DiscordApi&, deps)` / `handle_forum_reply(MessageCreate の d, deps)` / `finalize_forum_post(DiscordApi&, args)` と純関数 |

局所の補助 (TS に対応ファイルなし):

- `api/js_query.hpp` — クエリを JS の `Number(v)` / `Number.parseInt(v, 10)` と同じ規則で読む (learning-view / web-chat / blackbox が使う)。
- command_router の `SeedTask` / `resolve_seed_in_background` — TS の `seed: Promise<boolean>` の置き換え。route は分類器を呼ばずに
  「呼べば分類して started を返す」仕事を返し、呼び出し側 (Gateway) が背景のスレッドで解決して true のときだけ 👀 を付ける。
- command_router の `ROUTER_SLASH_COMMANDS` / `DEDICATED_SLASH_COMMANDS` / `DISCATIER_COMMANDS` / `ADMIN_SLASH_COMMANDS` —
  command_defs (w3-M) との一致をテストで確かめるための名前の表。
- command_router の `truncate_text` / `ja_locale_string` (`new Date(ms).toLocaleString("ja-JP")`、ローカル時刻) を公開してテスト。
- `CommandRouterDeps::openCore` (KG を開く関数) と `submitBackground` (fire-and-forget の積み先、既定 `background_executor().submit`) を注入可能にした。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| tests/discord-hook/command-router.test.ts (1〜9) | discord_hook.w4q_command_router |
| (新規) admin allowlist の境界 (未設定 / 空文字 / 一致 / 不一致 / 大文字違い / 末尾空白)・4 つの admin コマンドすべて | discord_hook.w4q_command_router |
| (新規) command_defs と route の name の一致・Discatier として登録したものが unknown command にならない | discord_hook.w4q_command_router |
| (新規) interaction JSON の入口 (argsText / enabled / DM)・返信本文の形・例外は internal error・平文とフォーラムの取り込み・seed・fire-and-forget | discord_hook.w4q_command_router |
| tests/discord-hook/forum-monitor.test.ts | discord_hook.w4q_forum_monitor (golden) |
| (新規) 偽 Discord での ThreadCreate (👀・リトライ・bot・親が forum でない) / 後続投稿 / finalize (転記・lock・archive・失敗) | discord_hook.w4q_forum_monitor |
| tests/core/projection/{e2e-axis1-flow, emotion-handlers, learning-handlers, e2e-axis3-submit-guard}.test.ts | core.w4q_message_input |
| (新規) submitMessage の golden (16 入力の応答と記録した utterance)・巨大/壊れた入力 | core.w4q_message_input |
| (TS テスト無し) blackbox / consensus / learning-view / web-chat ルート | api.w4q_routes (golden + 認可 + 境界) |

golden: `native/tests/golden/w4-Q-golden.gen.ts` → `native/tests/fixtures/w4q/`
(`web_assets.json` = 3 画面の sha256、`routes.json` = 合成データ (setup SQL) に対する 22 要求 + フォールバック 3 要求の status / Content-Type /
Content-Disposition / 本文、`slash.json` = routeSlashCommand の返信 21 件、`message_input.json`、`forum.json`)。
画面は `native/web/api/{consensus,chat,learning}.html` に Node が返した文字列をそのまま書き出した (learning.html は既存のものと同一だった)。
golden の比較は UUID を出現順の `<U1>` に、learning data の `generatedAt` を `<now>` に置き換えてから行う。TZ は UTC に固定。

## 3. 安全の対応表

| 防御・課金 | 場所 |
|---|---|
| admin-id allowlist: **空なら admin コマンド (discutere-kill / status / backup / persona-generate) は全拒否**、userId 無し・不一致は "admin only"。完全一致のみ (大文字違い・空白違いは拒否) をテスト | command_router.cpp `admin_guard` |
| mechanics-check / level-check が router に来ても utterance にしない (保護応答) | command_router.cpp |
| 平文の取り込みは discussionChannelIds (または親) に一致するときだけ。空は取り込まない (core に触れない) をテスト | command_router.cpp |
| フォーラムの受信境界は forum_event_gate / managed_forum_scope (w1-D / w2-J) が担う。forum_monitor は加えて親が GuildForum であることを REST で確かめる (取れなければ何もしない) | forum_monitor.cpp |
| interaction token は `RestSlashReplier` の中だけ (ログに出さない)。route の例外文 (DB のパス等) は返信にもログにも出さず "⚠️ internal error" | command_router.cpp |
| Discord への返信・まとめ転記は `allowed_mentions: {parse: []}` (メンションを解決しない) | command_router.cpp `slash_reply_body`、forum_monitor.cpp |
| Discord 操作はすべて DiscordApi (期限・応答上限・レート制限・同時実行上限・token の伏せ字)。finalize の失敗理由に token が出ないことをテスト | forum_monitor.cpp |
| fire-and-forget (backup / persona-generate / seed) は上限つき executor に積む。満杯なら開始しない旨を返す / 議論を立てない | command_router.cpp |
| consensus / blackbox の全ルートで admin role 必須 (401 / 403)、loopback は admin (middleware/auth) | consensus_routes.cpp, blackbox_routes.cpp |
| blackbox: 本文 JSON は 64KiB / 1000 要素 / 深さ 16、id は整数のみ (範囲外は 404)、verdict / state は列挙のみ | blackbox_routes.cpp |
| web-chat: loopback 信頼 (TS と同じ)、本文 64KiB (ルート上限 + JSON の上限 64KiB / 1000 要素 / 深さ 16)、表示名は 40 文字 (UTF-16) まで・個人データは表示名のみ。room は sanitize (英数 . _ - の 32 文字) | web_chat_routes.cpp |
| web-chat: 分類器の例外は seeded=false (落ちない)、議論が立っていれば分類器を呼ばない | web_chat_routes.cpp |
| 結論一覧は conclusion_cache だけを読む (KG を開かない。opener が呼ばれないことをテスト)。空のときだけ live マージ | learning_view_routes.cpp |
| 外部の声の個人アンカーは conclusions の serializer (w3-L) が仮名化 (`論者#xxxx`)、出所メタは attribution store | learning_view_routes.cpp |
| DB から読む行は上限つき (consensus 20 万行・256MiB、transcript / conclusions は各モジュールの上限) | consensus_routes.cpp ほか |

## 4. 移していないもの・挙動の差

- **Gateway の受信**: interaction / message / ThreadCreate の受信 (discord.js の Client.on) は後の担当。ここでは JSON を受ける関数
  (`handle_slash_interaction` / `handle_inbound_message_json` / `handle_forum_thread_create` / `handle_forum_reply`) にした。
  `handle_forum_thread_create` は分類 (LLM) を同期で待つので、Gateway 担当は background_executor に渡して呼ぶこと。
- **seed (Promise)**: TS は route の中で分類器を呼び始めていた (非同期)。C++ は SeedTask を返し、呼ぶまで分類しない。
  呼び出し側が `resolve_seed_in_background` で解決する。キューが満杯なら議論を立てない (縮退。warn を出す)。
- **fire-and-forget の満杯**: `/discutere-backup` と `/persona-generate` は executor が満杯だと「開始できませんでした」と返す
  (TS は常に「開始しました」)。
- **返信のメンション**: TS の `interaction.reply({content})` / `summaryChannel.send(text)` は allowed_mentions を付けていなかった。
  C++ は `{parse: []}` を付ける (LLM のまとめ・利用者の引数に @everyone 等が入っても通知しない)。Discord への本文の形の差であり、HTTP API の形は変えていない。
- **interaction 失敗時のログ**: TS は例外文をログに出していた。C++ はコマンド名だけ出す (DB のパス等を出さない)。
- **finalize の lock**: TS は `thread.manageable` (discord.js が権限から計算) のときだけ lock していた。C++ は常に PATCH locked=true を試し、失敗は無視する (結果は同じ)。
  フォーラム判定の親は discord.js のキャッシュの代わりに REST で取る。
- **starter の取得**: discord.js の `fetchStarterMessage` と同じく `GET /channels/{thread}/messages/{thread}`。timestamp は Discord の JSON のまま (TS は toISOString。経路では使わない)。
- **TS の型の穴の再現**: blackbox の verdict/state と web-chat の POST で本文が JSON の `null` のとき TS は TypeError → 500。C++ も 500 を返す。
  `/learning/conclusions?limit=` に NaN や小数を渡すと TS は SQLite の datatype mismatch で 500。C++ も 500 (負の値は TS と同じく全件 → slice で末尾を落とす)。
- **blackbox の limit**: `Math.min(200, Math.max(1, Number(q) || 50))` が小数のとき TS は小数のまま LIMIT に渡していた (SQLite が拒否しうる)。C++ は切り捨てる。
- **web-chat の本文上限**: TS (Hono) は上限なし。C++ は 64KiB (チャットの 1 発言として十分)。

## 5. 土台に欲しいもの / 人間の判断が要ること

- **土台に欲しいもの**: JS の `Number()` / `parseInt()` / `toLocaleString("ja-JP")` 相当 (`api/js_query.hpp`、command_router の `ja_locale_string`)。
  クエリの寛容な読み取りは他のルートにも散っているはず。
- **土台に欲しいもの**: `DiscordApi` に `get_message(channel, id)` はあるが、存在しない (404) と通信失敗を区別する戻り値の版が欲しい
  (starter のリトライで区別したい)。今は汎用の `request()` の例外で判定している。
- **人間の判断**: `/discutere-conclusions` の失敗文言に例外の内容 (`⚠️ 結論取得失敗: <message>`) を出すのは TS と同じにしたが、
  DB のパスが ephemeral 返信に出うる。固定文言にするかは判断が要る。
- **人間の判断**: seed / fire-and-forget のキュー満杯時の縮退 (議論を立てない・開始できない旨を返す) でよいか。

## 結果

`DI_RUN_TESTS=1 tools/build.sh /home/user/build-w4-Q` 相当 (ninja + ctest) で全 133 ctest が通過 (担当分 4: core.w4q_message_input / api.w4q_routes /
discord_hook.w4q_command_router / discord_hook.w4q_forum_monitor)。
