# port-notes w2-I — persona-engine の本体と常駐ワーカープール

job `cpp-node-20261030`。担当: persona-engine の rule engine (handler / engine / learning / index)、常駐ワーカープール
(pool / client) とその HTTP (callback / 制御)、Debate ランナー (director-live)、使い方ガイド、外部会話への参加 (service / routes)。

## 1. 機能の対応表 (TS → C++)

| TS | C++ | 名前空間 |
|---|---|---|
| src/persona-engine/engine/handler.ts | native/src/persona_engine/engine/handler.{hpp,cpp} | di::persona_engine::engine |
| src/persona-engine/engine/engine.ts | persona_engine/engine/engine.{hpp,cpp} (`Engine` クラス。tick は global_scheduler、発火は background_executor) | 同上 |
| src/persona-engine/learning/learner.ts | persona_engine/learning/learner.{hpp,cpp} | di::persona_engine::learning |
| src/persona-engine/learning/metrics.ts | persona_engine/learning/metrics.{hpp,cpp} | 同上 |
| src/persona-engine/index.ts | persona_engine/index.{hpp,cpp} (`create_persona_engine` / `PersonaEngineHandle` / `console_logger`、公開ヘッダの集約) | di::persona_engine |
| src/persona-engine/worker-pool/pool.ts | persona_engine/worker_pool/pool.{hpp,cpp} | di::persona_engine::worker_pool |
| src/persona-engine/worker-pool/client.ts | persona_engine/worker_pool/client.{hpp,cpp} | 同上 |
| src/api/worker.ts | native/src/api/worker.{hpp,cpp} (`worker_routes()` を "/internal" に mount、`set_worker_pool`) | di::api |
| src/api/worker-pool-control.ts | api/worker_pool_control.{hpp,cpp} (`worker_pool_control_routes()` を "/api" に mount) + 画面 native/web/api/worker-pool-control.html | di::api |
| src/api/guide-routes.ts | api/guide_routes.{hpp,cpp} (`create_guide_routes(config.discord)`) + 雛形 native/web/api/guide.html (`{{名前}}` の印) | di::api |
| src/external-discussion/service.ts | native/src/external_discussion/service.{hpp,cpp} | di::external_discussion |
| src/api/external-discussion-routes.ts | api/external_discussion_routes.{hpp,cpp} | di::api |
| src/discussion/director-live.ts | native/src/discussion/director_live.{hpp,cpp} (`DebateRunner`。discord.js → `DiscordApi` REST、ボタンは `ButtonInteraction` で受ける) | di::discussion |

main.cpp への結線 (mount 順・backend=worker-pool の分岐・`setRolePromptResolver` 等) は index.ts の担当 (最後の段階) が行う。
TS の module-level `let pool` (setWorkerPool / setWorkerPoolControl) は mutex で守った `shared_ptr` の late-binding にした。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| tests/persona-engine/engine.test.ts | persona_engine.engine |
| tests/persona-engine/safety-caps.test.ts | persona_engine.engine (+ 空の sessionId・再作成越しの cap) |
| (tick の start/stop は TS テスト無し) | persona_engine.engine (二重 start しない・stop 後は tick も fire_rule も走らない・再 start) |
| tests/persona-engine/rule-self-edit.test.ts | persona_engine.handler (golden `w2-I-golden.gen.ts` の handle_action.json に add_rule / remove_rule の全観点 + 境界) |
| handler の extractJson / FORBIDDEN_RULE_PATTERNS | persona_engine.handler (golden extract_json.json / forbidden.json) |
| tests/persona-engine/learning.test.ts | persona_engine.learning (golden learning.json / propose_pattern.json) |
| (worker-pool / api/worker / worker-pool-control は TS テスト無し) | persona_engine.worker_pool (偽 Lictor sidecar・起動停止の差し替え) |
| tests/flow/external-discussion.test.ts | external_discussion.service (+ golden external_service.json: LLM の system/prompt・結果・保存行) |
| (director-live は TS テスト無し) | discussion.director_live (偽 Discord REST) |
| (guide-routes は TS テスト無し) | api.guide_routes (golden guide.json: Node の HTML と完全一致) |

event-bridge / human-utterance / command-router 等は担当外モジュールのテスト (PersonaEngineHandle の型だけ使う)。

## 3. 安全の対応表

| 経路 | 防御 |
|---|---|
| `/internal/worker/*` | loopback 限定 (127.0.0.1 / ::1 / ::ffff:127.0.0.1 以外は 403 `{"error":"loopback only"}`)。境界テスト: 192.168.1.10 / ::ffff:192.168.1.10 / 10.0.0.1 / 127.0.0.2 / ::2 / 空は 403 |
| callback 本文 | 1 MiB (既定。ちょうどは通り +1 は 413 を実サーバで確認)、JSON は 1 MiB / 2 万要素 / 深さ 32 (超過・壊れは 400) |
| register の値 | lictorPort は 0..65535 の整数だけ (TS は任意の number)、lictorPid は 1..INT32_MAX の整数だけ (kill の対象になるため) |
| usage | 既知の有限な数値と空でない model だけ拾う (pick_usage) |
| Lictor /v1/keys | 127.0.0.1 固定・期限 10 秒 (接続 5 秒)・応答 64 KiB・リダイレクトを追わない |
| ターンの期限 | turnTimeoutMs (TS と同じ)。settle は registerSettleMs (TS と同じ)。期限切れ・注入失敗・stop で busy を戻す |
| ターン JSON のパス | reqId は [A-Za-z0-9_-] 128 文字まで (ファイル名に使うため。uuid なので実害なし) |
| external-discussion | 共有秘密 `Bearer <secret>` を `crypto::constant_time_equal` で比較。秘密 / service が無ければ 503。本文上限 160,000 バイト (`Router::body_limit` + RouteOptions。ちょうどは通り +1 は 413 を実サーバで確認)、JSON は 160,000 バイト / 2 万要素 / 深さ 16。同時 4 本 (5 本目 429) |
| external-discussion の LLM 応答 | JSON 256 KiB / 2000 要素 / 深さ 16、text は 1200 文字 (UTF-16) まで (TS と同じ) |
| handler の LLM 応答 | JSON 1 MiB / 5 万要素 / 深さ 32、波括弧の候補は 256 回まで試す (病的入力で O(n²) にしない。境界テスト: ちょうど / +1) |
| 禁則パターン・PROPOSE_PATTERN | std::regex を使わず線形の走査 (golden で Node の正規表現と一致を確認。2 MB の空白でも線形) |
| Debate ランナー | bot token は DiscordApi の中だけ (ログに出さない)。webhook 実行・interaction 応答は Authorization を付けない。ボタンは発行したチャンネルからのものだけ有効 (コピーした custom_id で別スレッドを操作できない) |
| 使い方ガイド | config の値は TS の esc() と同じ 4 文字のエスケープ。`{{名前}}` は 1 回の走査で置換 (値の中の `{{...}}` は再展開しない。golden で確認) |

## 4. 移していないもの・挙動の差

- `AnthropicSdkClient` の再 export は移していない (w1-B の判断どおり。API キー / OAuth で直接呼ぶ経路)。
- engine: stop() の後に LLM の応答が戻った発火は議論へ書き込まない (rule_log に skip "engine stopped")。TS は書き込む。
  tick ジョブは実行中 1 本 + 待ち 1 本まで (executor のキューを tick で埋めない)。TS は毎秒の tick が重なると busy ログを出し続ける。
  消える場合があるのは「重なった tick の engine busy ログ」だけで、発火の可否は変わらない。
- engine: 空文字の sessionId / event_kind / target は TS と同じく「無い」(falsy) と扱う。
- handler: add_rule の tick_sec / cooldown_sec が小数 (例 5.5) のとき整数に切り捨てて保存する (TS は SQLite に REAL のまま入る)。
- pool: dispatch は busy の判定と同時に busy を立て、callback の待ちを注入の前に登録する (TS は注入後。注入中に戻った callback を取りこぼさない・同じワーカーへの同時 dispatch を確実に 1 本にする)。
  Lictor に繋がらないときの文言は TS と同じ "fetch failed"。stop_worker は TS と同じく待っているターンを起こさない (期限で終わる)。
- pool: ログの文言は TS と同じ (pid が無いときは "pid=undefined")。console.log の第 2 引数 (meta) は出さない。
- index: console_logger は "[persona-engine] <msg> <meta の JSON>" (Node の util.inspect 書式ではない)。
- external-discussion: LLM 応答が JSON でないときの例外文は parse 失敗の理由ではなく "invalid discussion response" (HTTP 応答は TS と同じ 503)。
- director-live (TS の不具合を直した点): TS は `ensureChannelWebhook(channelId, botToken)` / `resolveWebhookTarget(channelId, botToken)` と
  引数を逆に渡していた (poster の引数は (botToken, channelId))。このため TS では bot token を URL のパスに入れて Discord に送り、
  チャンネル id を Authorization に入れていた (= /debate の投稿は失敗し、token が Discord 側のアクセスログに残りうる)。
  C++ は他の呼び出し元 (flow/discord-live, discussion-bridge) と同じく「resolve_webhook_target → 親チャンネルで ensure_channel_webhook → thread_id 付きで投稿」にした。
- director-live: start() は議論が終わるまで呼んだスレッドで進む (gateway 側は background_executor に渡す)。続行の問いを送れるチャンネルは
  discord.js の isTextBased() かつ send を持つ種類 (テキスト / DM / ボイス / アナウンス / スレッド / ステージ) で、フォーラム等は TS と同じく問わずに停止。
  DebateRunner を破棄すると待っている問いは停止扱いで起きる。
- guide: HTML は TS と同じく作成時に 1 度だけ組み立てる。

## 5. 土台に欲しいもの・人間の判断が要ること

- 人間の判断: TS 版 director-live の引数取り違え (上記) を TS 側でも直すか。/debate を本番で使っていたなら bot token のローテーションを検討。
- 人間の判断: `/internal/worker/register` が受け取る lictorPid を kill の対象にしている (TS から同じ)。loopback 限定だが、同一ホストの別プロセスが
  任意の pid を登録できる。worker 側に共有秘密を持たせるか、spawn した pid の子孫だけを kill するかは設計判断。
- 人間の判断: worker callback の本文上限は既定 1 MiB にした (TS は無制限)。1 ターンの発話が 1 MiB を超える運用があれば上げる。
- 土台に欲しいもの: `http::Request` の JSON 読み取りで失敗理由 (上限超過 / 構文) を区別できる API。Scheduler::cancel が「走っている最中の
  コールバックの終了を待つ」版 (今は世代番号と weak_ptr で古いタイマーを無効化している)。
