# port-notes w5-U: Discord のライブ進行 (flow/discord-live) と Gateway の結線 (discord-hook/gateway)

job `cpp-node-20261030`。担当 TS 2 ファイルを C++ に移した。

## 1. 機能の対応表 (TS → C++)

| TS | C++ | 備考 |
|---|---|---|
| src/flow/discord-live.ts | native/src/flow/discord_live.{hpp,cpp} (`di::flow`) | `start_forum_flow` / `handle_paper_review_reply` / `handle_paper_review_approval` / `continue_paper_review_with_flow` / `cancel_paper_review` / `handle_interrupted_discussion_reply` / `handle_forum_flow_reply` / `has_paper_review` / `has_sparring_session` / `reset_flow_live_for_test` (TS の `_resetFlowLive`)。`approval_guide` はテスト用に公開 |
| src/discord-hook/gateway.ts | native/src/discord_hook/gateway.{hpp,cpp} (`di::discord_hook`) | `start_discord_gateway(DiscordGatewayDeps)` → `DiscordGateway` (`stop` / `finalize_forum_post` / `react_to_message` / `sweep_unseeded`)。純関数 `is_text_like_attachment` / `modal_field_values` / `positive_number_field` |

### discord.js の Client を何に置き換えたか

| TS (discord.js) | C++ |
|---|---|
| `new Client({intents, partials})` + `client.login` | w4-T の `GatewayClient` (既定 intents は TS と同じ 38401)。`dispatch_in_background=false` にして制御スレッドで振り分けだけを行う |
| `client.once(Events.ClientReady)` | `READY` を 1 度だけ処理 (`ready_started`)。再 identify の READY では繰り返さない |
| `bindManagedForumEvents(client, ...)` | w2-J の `bind_managed_forum_events` (MESSAGE_CREATE / MESSAGE_REACTION_ADD / INTERACTION_CREATE / THREAD_CREATE はすべて門を通る) |
| `client.channels.fetch` (キャッシュあり) | `DiscordApi::get_channel` + 同一性 (id / guild / parent / type) だけの TTL キャッシュ (既定 5 分・2000 件)。境界の判定は毎回 managed_forum_scope が行う |
| `thread.fetchStarterMessage()` (1 回リトライ) | `GET /channels/{t}/messages/{t}` を 2 回まで (間 500ms) |
| `interaction.reply / update / showModal / deferReply / editReply` | interaction callback type 4 / 7 / 9 / 5 と `PATCH @original` (局所の `Ix`)。reply には `allowed_mentions: {parse: []}` を付ける (w4-Q と同じ) |
| `msg.reply(text)` (調整指示の了解) | `post_discord_channel(replyTo=msg.id)` (`allowed_mentions.replied_user=false` = 返信先に通知しない) |
| `msg.react` / `reaction.message.channelId` 等 | `DiscordApi::create_reaction` / Gateway の `d` |
| `createDebateRunner({client, botToken, ...})` | `discussion::create_debate_runner` (DiscordApi を共有)。ボタンは `ButtonInteraction` に詰めて渡す |
| `new MonitorCard({client,...}).start()` | `MonitorCard(MonitorCardDeps{api, workspace, guild})` (`monitorCards=false` で置かない: テスト用) |
| `void startForumFlow(...)` 等の投げっぱなし | `flowLive.submitBackground` (既定 `background_executor().submit`) |
| `setTimeout` (ペーパーの自動開始) | `global_scheduler().after` → 背景キュー → `run_serial` |

### 依存の注入 (並行担当・未移植のもの)

- **flow/entry-discord.ts** (並行担当 w5-R): `parseForumEntry` / `parseRoundsTurns` / `parseOpponents` は
  `DiscordGatewayDeps::entryParsers` (std::function) で注入する。ヘッダは参照しない。未設定なら「タイプ未判定」(= 議論タイプ選択メニューを出す) と「指定なし」として動く。**main で結線すること。**
- **flow/information-gate-runner.ts** (未移植): discord-live が使うのは `resolveDebatabilityGate` だけ
  (`gateBeforeFlow` を呼ぶ `prepareInformationBeforeForumFlow` / `legacyAutoCrawlBeforeForumFlow` は TS でもどこからも呼ばれていない死んだコード)。
  `resolveDebatabilityGate` は 7 行 (config.flow.debatability.enabled・議論/改善のみ・withCostLog(location="debatability-gate")・minArmableIssues) なので
  `build_forum_paper_draft` の中に同じ解決を局所に書いた。information-gate-runner が移されたらそちらへ置き換える。
- テストのための注入点 (既定は本物): `FlowDiscordDeps::dispatch` (dispatch_flow)、`prepareUserVoices` (Steam / Voluptas へ出る)、
  `expandNotionLinks` (Canalis の sidecar を起こす)、`submitBackground`。`DiscordGatewayDeps::rest` / `gatewayOptions` / `connect` / `monitorCards` /
  キューの大きさ / `sleep`。

### キューの設計 (`dropped_queue_full` の扱い)

gateway_client の `on_dispatch` は制御スレッドで呼ばせ (`dispatch_in_background=false`)、`handle_dispatch` が `d` を見て 2 本の上限つき executor に振り分ける:

| キュー | 既定 | 入るもの |
|---|---|---|
| 起動系 `discord-start` | 2 スレッド / 1024 件 | READY、THREAD_CREATE (議論の起動)、INTERACTION_CREATE (選択メニュー・進行量・slash。3 秒の応答期限)、✅ の MESSAGE_REACTION_ADD (承認) |
| 通常 `discord-events` | 4 スレッド / 512 件 | MESSAGE_CREATE、✅ 以外のリアクション |

- 通常のイベント (投稿の洪水) が溢れても議論の起動・承認は落ちない (テスト「通常キューが満杯でも起動系は落ちない」)。
- 起動系が満杯で落ちたときは `log::error` + `event_stats().priorityDropped`。通常は `warn` + `normalDropped`。
- ハンドラ自身は REST (境界の fetch・ack・メニュー投稿) までで、LLM を待つ仕事 (フローの起動・ペーパー調整・診断・debate・crawl) は
  `submitBackground` に渡す。そこが満杯のとき: フローの起動は議論タイプ選択メニューを出し直して再試行を促す、承認はレビュー待ちに戻して
  「もう一度開始」を案内、診断は失敗の返信、返信の処理は warn して捨てる。
- mechanics-check / level-check は 3 秒の期限を守るため **先に defer してから** 背景で診断する (TS は handler 内で defer していたが、同じスレッドで待つと期限を超えうる)。
- ペーパー調整モーダル (`paper-gap`) も defer してから背景で処理し、`editReply` で結果を返す。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| tests/flow/paper-review.test.ts の「handlePaperReviewApproval は承認待ちが無ければ no-op」 | flow.w5u_discord_live (✅ 承認 / 破棄 / フロー再提案の続行) |
| tests/discord-hook/mechanics-check.test.ts / level-check.test.ts の gateway 文字列検査 (診断コマンドが汎用ルータより先・失敗は「reply delivery failed」だけ) | 構造は gateway.cpp の `handle_command` (診断を先に分岐し、例外はログに内容を出さない)。ソース文字列の検査は移さない |
| tests/discord-hook/forum-event-gate.test.ts の gateway 文字列検査 (bindManagedForumEvents を使い client.on で直接受けない) | discord_hook.w5u_gateway (管理外・なりすまし guild・別スレッドの component・fail closed の各ケース) |
| (新規) Node と同じ投稿本文 (golden) | flow.w5u_discord_live (golden 2 件) |
| (新規) 偽 Gateway (WebSocket) の E2E: READY → THREAD_CREATE → 起動 → webhook → 結論 → まとめ転記 + lock/archive、再送の重複排除、token をログに出さない | discord_hook.w5u_gateway |

golden: `native/tests/golden/w5-U-discord_live.gen.ts` → `native/tests/fixtures/w5u/discord_live.json`
(globalThis.fetch を差し替えて Discord REST を受け、LLM は常に失敗する偽物、観点タグ「機密」で外部収集を避ける)。
学習の Core 未設定と、レビューゲートの「準備 → 草案 → 声の準備 → 案内 → 戻す → 相づち → 調整 (LLM 失敗) → 受付確認 → 開始」の投稿列と、
各返信の後の投稿数・レビュー状態が Node と一致する。

## 3. 安全の対応表

| 防御・入力経路 | C++ |
|---|---|
| 受信は Di 管理フォーラムだけ (guild ID + 親 forum ID、空は全拒否、取得失敗は拒否) | 4 種のイベントすべて `ForumEventGate` を通す。THREAD_CREATE の `d` の guild を偽っても REST の同一性で落ちる (テスト)。フォーラム名が空・解決失敗の guild は登録しない |
| reply 投稿は宛先によらず無視・bot / webhook の投稿は無視 | 門 (`has_reference` / `author_bot`) + ハンドラでも `message_reference` / `author.bot` を再確認。webhook の投稿 (`webhook_id`) も bot 扱い |
| 調整指示は通常投稿の bot 明示メンションのみ | `mentions` に自分の id がある通常投稿だけ。レビュー中のスレッドはメンションがあってもレビューへ (TS と同じ順) |
| bot 自身のリアクションは承認・スコアに使わない | `member.user.bot` または `user_id == READY の自分の id` を bot とみなす (discord.js は自分を ClientUser としてキャッシュしていた) |
| component は自分のスレッドの待ちだけを操作できる | 門の `component_matches_channel` (別スレッドの `flow-pick:<id>` は捨てる。テスト) |
| admin コマンド: allowlist 未設定は全拒否 | w4-Q の `route_slash_command` に渡す (テストで「未設定」「不一致」を確認) |
| bot token をログに出さない | Gateway・REST は token を出さない (w4-T / w1-D)。ここでも token・webhook token・interaction token をログ・例外文に入れない。E2E テストでログ全体に token / webhook token が無いことを確認 |
| 外部入力の上限 | Gateway のフレームは gateway_client の上限 (8MiB / 50 万要素 / 深さ 64)。モーダルの入れ子は深さ 8 まで。待ち表 (議論タイプ選択・進行量) は `maxPendingStarts` (既定 1000) で頭打ち。チャンネルのキャッシュは 2000 件。REST 応答は DiscordApi の上限 |
| 同時実行の上限 | 起動系・通常のキューは上限つき executor。LLM の仕事は background_executor (上限つき)。スレッドごとの直列化は `run_serial` |
| 機密/内部タグの外部の声遮断 | discord-live で `can_collect_external` (ユーザーの声の収集・調整のクロール) と `gate_external_voices` (調整の材料) を TS と同じ箇所で通す |
| 投稿のメンション | 通知・結論は TS と同じく `post_discord_channel` (allowed_mentions を付けない = TS と同じ)。interaction の返信は `{parse: []}` |

## 4. 移していないもの・挙動の差

- **壁打ち・学習の dispatch**: TS の `parseFlowKind` は学習・壁打ちのラベルを持たない (w4-P の dispatch も同じ) ため、`dispatchFlow` に
  `flow: "sparring"` / `"learning"` を渡すと FlowTypeRequiredError になり、スレッドに「⚠️ フロー実行中にエラーが発生しました: …」が出る
  (学習は Core 未設定の案内が先)。C++ も同じ (テスト「本物の dispatch は TS と同じく壁打ちを受理しない」)。discord-live の壁打ち経路
  (session の登録・返信の橋渡し・/終了) は移してあり、session を直接作る偽 dispatch で確かめた。復活させるかは dispatch 側の判断。
- **死んだコード**: `prepareInformationBeforeForumFlow` / `legacyAutoCrawlBeforeForumFlow` (TS でも未使用) は移さない。
- **承認後の議論の起動が背景キュー満杯で積めないとき**: TS は必ず起動していた (Promise)。C++ はレビュー待ちに戻し「混雑のため開始できませんでした。もう一度「開始」」を投稿する。
  同様にフローの起動が積めないときは議論タイプ選択メニューを出し直す。
- **進行中の印**: TS の `runDiscussionDispatch` は同期に runningDiscussions へ入れてから await する。C++ は背景に積む前に印を付ける (承認直後の返信で下書きからレビューが復活しない)。
- **イベント間の順序**: executor に渡すので保証しない (discord.js も await で交互に進んだ)。同じスレッドの返信は `run_serial` が届いた順に通すが、
  2 本の返信が別々のワーカーで run_serial に着く順はわずかに入れ替わりうる (TS の非同期処理と同程度)。
- **`run_serial` の待ち**: 同じスレッドで LLM の調整が走っている間、後続の返信を処理するワーカーは待つ (TS は await で待っていた)。
  `continue_paper_review_with_flow` (flow-pick の選び直し) もこの待ちに入るので、調整中に選び直すと interaction の 3 秒期限を超えうる (TS と同じ)。
- **interaction の reply** に `allowed_mentions: {parse: []}` を付けた (w4-Q と同じ方針。LLM や利用者の文字列で通知しない)。
- **sweepUnseeded**: 👀 の既読判定は REST の `reactions[].me` を使い、`me` が無いときだけ反応ユーザーを取りに行く (discord.js の partial 対応と同じ結果)。
  走査件数 `limit` は 1〜100 に丸める (Discord の上限)。
- **ゲーム感想・crawl・平文の取り込み**: TS でも門 (管理フォーラムのみ) の後ろにあるため、管理フォーラム外の投稿には反応しない (TS と同じ)。経路は移した。
- **README / ログの文言**: `logged in as <tag>` の tag は username だけ (discriminator は廃止済み)。

## 結果

`DI_RUN_TESTS=1` 相当 (ninja + ctest) でビルドディレクトリの ctest 全体 137/137 通過 (担当分: flow.w5u_discord_live 12 件、
discord_hook.w5u_gateway 15 件。担当分を 5 回連続で実行して失敗なし)。`ctest -j2` の 1 回目で discussion.director_live (w2 担当分、
時間に依存するテスト) が 1 度だけ落ち、再実行では通った。

## 5. 土台に欲しいもの / 人間の判断が要ること

- **人間の判断**: 起動系キューも満杯 (1024 件) で THREAD_CREATE を落とした場合、そのスレッドの議論は起動しない (Discord は再送しない)。
  `event_stats().priorityDropped` を監視するか、後から `sweep` 相当で拾い直すかを決めてほしい。
- **人間の判断**: `⚠️ フロー実行中にエラーが発生しました: <例外文>` / `⚠️ 調整の反映に失敗しました: <例外文>` は TS と同じく例外文をスレッドに出す。
  DB のパス等が出うるので固定文言にするかは判断が要る (w4-Q の /discutere-conclusions と同じ論点)。
- **main の結線**: `entryParsers` (w5-R の entry-discord)、`flowLive` (LLM / openCore / listExternalVoices / sentimentClients / getYoutubeApiKey)、
  `onReaction` (合意スコア)、`onGameFeedback`、`mechanicsCheck` / `levelCheck`、`set_gateway_sweeper` に `sweep_unseeded` を、facilitator の
  収束に `finalize_forum_post` を結ぶこと (TS の index.ts と同じ)。
- **土台に欲しいもの**: `Executor` に「空になるまで待つ」(テストの drain) があると、各担当がポーリングを書かずに済む。
- **土台に欲しいもの**: `DiscordApi` に `get_channel` の結果のうち不変な同一性だけを持つキャッシュ (ここでは局所に置いた)。毎メッセージ 2 回の
  REST (スレッド + 親フォーラム) を避けるため。
