# port-notes w3-M: 進行役 (facilitator)・合意スコア・自動 seed・admin API・slash コマンド定義

job `cpp-node-20261030`。担当 TS 17 ファイルをすべて C++ に移した (Node に残したものは無い)。

## 1. 機能の対応表 (TS → C++)

| TS | C++ | 備考 |
|---|---|---|
| src/persona-engine/facilitator/facilitator.ts | native/src/persona_engine/facilitator/facilitator.{hpp,cpp} | `Facilitator` (start/stop/tick_once/converge_now)、`evaluate`、`FACILITATOR_PERSONA()`。deps は shared_ptr (core / llm / contextProvider / personas / logger)、`tuning` と `onConverged` は std::function |
| src/persona-engine/facilitator/index.ts | native/src/persona_engine/facilitator/index.hpp | 再 export (facilitator.hpp + prompts.hpp) |
| src/persona-engine/scoring/consensus-scorer.ts | native/src/persona_engine/scoring/consensus_scorer.{hpp,cpp} | `ConsensusScorer`、`parse_verdicts` (TS の非公開関数を公開してテスト) |
| src/discussion-seed/seed.ts | native/src/discussion_seed/seed.{hpp,cpp} | `seed_headless_discussion`、`build_opening` |
| src/discussion-seed/scheduler.ts | native/src/discussion_seed/scheduler.{hpp,cpp} | `AutoSeedScheduler` (start/stop/trigger)。Steam の取得先は `storeTrendSource` で差し替え可 |
| src/api/admin-routes.ts | native/src/api/admin_routes.{hpp,cpp} | `admin_routes()` + late-binding `set_persona_engine` / `get_persona_engine` / `set_facilitator` / `set_gateway_sweeper` (mutex で保護) |
| src/api/consensus-persona-routes.ts | native/src/api/consensus_persona_routes.{hpp,cpp} | |
| src/api/gap-creation-routes.ts | native/src/api/gap_creation_routes.{hpp,cpp} | `parse_gap_creation_input` を公開 |
| src/discord-hook/command-defs.ts | native/src/discord_hook/command_defs.{hpp,cpp} | `DISCORD_COMMAND_DEFS()` (Json)。`option_type::{String,Integer,Boolean}` |
| src/discord-hook/register-commands.ts | native/src/discord_hook/register_commands.{hpp,cpp} | w1-D の `DiscordApi::bulk_overwrite_commands` で PUT |
| src/discord-hook/level-check.ts | native/src/discord_hook/level_check.{hpp,cpp} | `handle_level_check_command(SlashInteraction, InteractionResponder&, deps*)` |
| src/discord-hook/mechanics-check.ts | native/src/discord_hook/mechanics_check.{hpp,cpp} | 同上 |
| src/level-check/tool.ts | native/src/level_check/tool.{hpp,cpp} | Json を受けて検証する版と、検証済み要求を受ける版 |
| src/mechanics-check/tool.ts | native/src/mechanics_check/tool.{hpp,cpp} | 同上 |
| src/discord-hook/discussion-bridge.ts | native/src/discord_hook/discussion_bridge.{hpp,cpp} | token ごとに DiscordApi を 1 つ保持 (鍵は sha256)。テスト用に `set_discussion_bridge_api_factory` |
| src/discord-hook/directive-handler.ts | native/src/discord_hook/directive_handler.{hpp,cpp} | active KG を開く版と、開いた KG を受ける版 |
| src/discord-hook/monitor-card.ts | native/src/discord_hook/monitor_card.{hpp,cpp} | discord.js の Client の代わりに `shared_ptr<DiscordApi>`。`build_monitor_embed` (EmbedBuilder.toJSON と同形) |

局所の補助 (TS に対応ファイルなし):

- `persona_engine/facilitator/periodic.hpp` — `PeriodicTask`。`setInterval` + `running` フラグの置き換え。
  `global_scheduler().every` で起こし、仕事は `background_executor().submit` で走らせる。前回が終わっていなければその周期は飛ばす (TS と同じ)。
  キュー満杯なら飛ばして warn。`stop()` は以後の起こしを止め、走っている仕事の終わりを待つ (仕事の中から呼ばれたら待たない)。
  facilitator / consensus scorer / auto-seed scheduler / monitor card が使う。デストラクタでも stop するので、破棄後に仕事が走らない。
- `persona_engine/facilitator/js_value.hpp` — LLM 応答の値を JS の `String(v)` / `Number(v)` と同じ規則で読む (golden で Node と一致を確認)。
- `discord_hook/slash_interaction.{hpp,cpp}` — `SlashInteraction` (INTERACTION_CREATE の `d` から commandName / options / user を読む) と
  `InteractionResponder` (deferReply / editReply の口)。REST 実装 `RestInteractionResponder` は
  `create_interaction_response` (type 5 + flags 64) と `edit_original_interaction_response` (添付は multipart)。Gateway の担当はこれを使って受信をつなぐ。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| tests/persona-engine/facilitator.test.ts | persona_engine.w3m_facilitator_tests (evaluate・拡張・止揚収束・session の ended_at) |
| (新規) facilitator の converge_now / onConverged / 調整指示の注入 / 除外発話 / 人間の下駄 / maxPersonas と aufhebung-only / 壊れた応答 / start・stop | persona_engine.w3m_facilitator_tests |
| (新規) consensus scorer (TS テスト無し) | persona_engine.w3m_facilitator_tests (parse_verdicts の境界・採点・👍・失敗) |
| (新規) discussion-seed (TS テスト無し) | discussion_seed.w3m_scheduler_tests |
| (新規) admin / consensus-persona / gap-creation ルート (TS テスト無し) | api.w3m_admin_routes_tests |
| tests/discord-hook/level-check.test.ts, mechanics-check.test.ts | discord_hook.w3m_discord_tests (slash 入力・defer→llm→edit・失敗の返信・他コマンド・転送失敗) |
| tests/level-check/tool.test.ts, tests/mechanics-check/tool.test.ts の tool 部分 | discord_hook.w3m_discord_tests (golden・検証失敗で LLM を呼ばない・llm_failed に秘密を出さない・invalid_response・期限・use_level_check) |
| (新規) command-defs / register-commands / discussion-bridge / directive-handler / monitor-card | discord_hook.w3m_discord_tests |

golden: `native/tests/golden/w3-M-golden.gen.ts` → `native/tests/fixtures/w3m/`
(`command_defs.json` = `JSON.stringify(DISCORD_COMMAND_DEFS)` そのもの、`monitor_embeds.json`、`js_values.json`、`diagnostic_tools.json`)。

移していない TS テストの観点:
- level-check.test.ts / mechanics-check.test.ts の `routeSlashCommand` ガード (command-router は担当外) と、
  gateway.ts のソース文字列を読む検査 (gateway は担当外)。
- tool.test.ts の `parseLevelCheckResponse` 単体の観点は w2-J の design_diagnostic テストにある。tool.ts のソースが flow/core 等を import しない検査は C++ では include の形で守っている (level_check/tool.cpp は level_check と design_diagnostic だけを include)。

## 3. 安全の対応表

| 防御・課金 | 場所 |
|---|---|
| admin role 必須 (userId 無し 401 / role≠admin 403、w1-F の `middleware::require_admin`)。全 15 ルートで両方をテスト。Cache-Control は guard を通った後だけ | admin_routes / consensus_persona_routes / gap_creation_routes |
| late-binding (engine / facilitator / sweeper) は mutex で保護、未注入は 503 | admin_routes.cpp |
| gap 作成: Content-Type が application/json 以外は 415、入力は厳格 (空白だけ・型違い・null の gameId は 400)、gameId は同 workspace の games に存在すること | gap_creation_routes.cpp |
| DB の失敗 (パスや SQL を含みうる) は応答に出さず 503 固定文言 | consensus_persona_routes / gap_creation_routes |
| persona DB は readonly + fileMustExist + busy_timeout 2000 で開く (無いときに作らない) | consensus_persona_routes.cpp |
| HTTP 本文は既定 1 MiB (ルータ)、JSON は `Request::json` の上限。ちょうど上限・深い入れ子をテスト | admin_routes |
| LLM 応答 JSON の上限: 合意判定 1 MiB / 5 万要素 / 深さ 32 (ちょうど・+1・深い入れ子をテスト)、facilitator は prompts の `llm_json_limits` | consensus_scorer.cpp, facilitator.cpp |
| slash の inline JSON: Discord 上限 6000 (UTF-16) を超えたら拒否、parse は 24KB / 2 万要素 / 深さ 64 | discord_hook/{level,mechanics}_check.cpp |
| 診断の失敗文言に LLM のエラー内容を出さない (`secret-token` が返信に出ないことをテスト)、mentions は `{parse:[]}` | 同上 (diagnostic_reply は w1-D) |
| 診断の LLM は conversationOnly + 期限 + 同時実行上限 (w2-J の invoke_diagnostic) | level_check/tool.cpp, mechanics_check/tool.cpp |
| playModel を持つ mechanics 要求は LLM を呼ぶ前に use_level_check | mechanics_check/tool.cpp (validate) |
| bot token はログ・戻り値に出さない (bridge の結果に token が出ないことをテスト)。token ごとの DiscordApi は sha256 を鍵に保持 | discussion_bridge.cpp |
| Discord 操作はすべて DiscordApi (期限・応答上限・レート制限・同時実行上限・token の伏せ字) | bridge / monitor_card / register_commands / slash_interaction |
| 調整指示: 本文 300 (UTF-16) 上限で切って保存、author_id は prompt に出さない (facilitator の prompt に出ないことをテスト) | directive_handler (facilitator_directives は w1-D) |
| 定期実行: 前回が終わるまで重ねない、executor 満杯は飛ばす、stop は終わりを待つ | periodic.hpp |
| DB から読む行は上限つき (facilitator / scorer 20 万行・256MiB、admin の scene 集合 10 万行) | 各 .cpp |
| headless の議論 (scene=gap:) は Discord に出さない (skipped) | discussion_bridge.cpp |

admin-id allowlist (未設定なら admin コマンド全拒否) を扱うのは command-router (担当外)。担当の slash 定義・
level-check / mechanics-check は admin コマンドではない (TS でも allowlist を見ない)。

## 4. 移していないもの・挙動の差

- **Gateway の受信**: interaction の受信 (discord.js の `InteractionCreate`) は後の担当。ここではハンドラ関数
  (`SlashInteraction` + `InteractionResponder`) の形にした。monitor-card の起動 (ClientReady) も同様に呼び出し側。
- **monitor-card のキャッシュ**: discord.js の `guild.channels.cache` の代わりに毎回 `GET /guilds/{id}/channels`。
  既存カードの判定は `GET /channels/{id}/messages?limit=100` の生 JSON (author.id と embeds[].title) で行い、
  並べ替えは snowflake の値 (= createdTimestamp の順)。取得失敗は TS と同じく null 扱いで続ける。
- **facilitator の並行性**: TS は単一スレッドで、tick と converge_now (HTTP) は await の境で入れ子になり得た。
  C++ では tick は executor のスレッド、converge_now は HTTP のスレッドで走る。セッション状態 (`states`) は mutex で守る。
  同じ議論への二重収束の防止は TS と同じく「gap が terminal なら何もしない」だけ (追加の排他は入れていない)。
- **consensus scorer の表の確保**: TS は facilitator / reactions が作る表 (`utterance_exclusions`・`discord_message_map`・
  `opinion_scores`) を前提にしていた。C++ では scorer 単独でも落ちないよう、構築時に冪等に確保する (中身・形は同じ)。
- **TS の型の穴に当たる入力**: `POST /admin/rules/enabled` 等で本文が JSON の `null` のとき、TS は `null.enabled` の TypeError で 500。
  C++ も例外 → サーバが 500 "Internal Server Error" (同じ)。
- **診断の返信のキー順**: 返信本文 (コードブロック / 添付) は `design_diagnostic::to_json` (w1-D / w2-J) の順で書かれるため、
  所見の `kind` の位置が Node (末尾) と違う (中身は同じ。w2-J の port-notes にある既知の差)。テストはキー順を無視して比べる。
  slash 定義・状態カードの embed はキー順まで一致 (byte 一致の golden)。
- **seed-sweep の sweeper**: `GatewaySweeper = function<SweepResult(optional<double>)>`。gateway 側の実装は後の担当。

## 5. 土台に欲しいもの / 人間の判断が要ること

- **土台に欲しいもの**: 定期実行の部品 (`PeriodicTask` 相当: scheduler で起こして executor で走らせ、重ねない・stop で終わりを待つ)。
  今は `persona_engine/facilitator/periodic.hpp` に局所に置き、4 モジュールで共有している。他の担当 (backup・kg-sync・cost relay 等) にも同じ形があるはず。
- **土台に欲しいもの**: JS の `String(v)` / `Number(v)` を Json に対して行う関数 (`js_value.hpp`)。LLM 応答の寛容な読み取りで各所に要る。
- **土台に欲しいもの**: `DiscordApi` に `get_channel_messages(channel_id, limit)` と `delete_message`、embed を含む `Message` の struct。
  monitor-card では汎用の `request()` で生 JSON を読んでいる。
- **人間の判断**: admin の `POST /admin/sessions/:id/converge` は LLM まとめを HTTP ハンドラの中で同期に待つ (TS も await していた)。
  議論が多いと応答が数分かかり、HTTP の worker を占有する。background に回して即応答する形に変えるかは UI の挙動が変わるので判断が要る。
