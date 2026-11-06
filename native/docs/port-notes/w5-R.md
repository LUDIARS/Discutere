# port-notes w5-R: Web のフロー画面のルート・情報ゲートの実行・Slack の入口

job `cpp-node-20261030`。担当の TS 6 ファイル (約 2,600 行) をすべて C++ に移した (Node に残したものは無い)。
golden 生成: `npx tsx native/tests/golden/w5-R-golden.gen.ts` → `native/tests/fixtures/w5r/{entry,flow_prep,synthetic,gate,routes}.json`。

## 1. 機能の対応表 (TS → C++)

| TS | C++ | 備考 |
|---|---|---|
| src/flow/entry-discord.ts | native/src/flow/entry_discord.{hpp,cpp} | `parse_forum_entry` / `parse_rounds_turns` / `parse_opponents` / `handle_forum_flow_post`。正規表現 (`/ラウンド\s*[:：=]\s*(\d+)/i`・`/\brounds?…/i`・`/(?:相手\|opponent)\s*[:：=]\s*(.+)/i`) は線形の走査で同じ一致 (JS の `\s`・`\b`・`.` の行末記号・貪欲な `\s*` の後戻り) を書いた。`ForumTagMapping::flowKindTagNames` は TS のオブジェクトの挿入順を保つため pair の配列 |
| src/flow/information-gate-runner.ts | native/src/flow/information_gate_runner.{hpp,cpp} | `gate_before_flow(FlowGateArgs)` / `resolve_debatability_gate(kind, sessionId, llm)`。`DebatabilityGateOptions` は with_cost_log で包んだ LLM を shared_ptr で持ち、`to_paper_gate()` で build_paper_draft の `PaperDebatabilityGate` にする。テスト用に `collectAndImportFn` / `evaluateFn` の差し替え口を追加 (既定は本物。密度評価は常に blackbox で包む) |
| src/flow/web/flow-prep.ts | native/src/flow/web/flow_prep.{hpp,cpp} | `FlowWebDeps` (単一定義) / `resolve_youtube_api_key` / `web_paper_gate_enabled` / `build_auto_crawl_spec` / `build_learning_crawl` / `prepare_information_before_flow` / `legacy_auto_crawl_before_flow`。`FlowWebDeps` に外部 I/O の差し替え口 (collector・取込・情報ゲートの取得と評価・仕様書の fetch とサイドカー・gh token・Notion クロール・Anatomia CLI) を追加 (既定は本物) |
| src/flow/web/routes.ts | native/src/flow/web/routes.{hpp,cpp} | `flow_routes()` (17 ルート、経路・JSON の形・status は TS と同じ) / `set_flow_web_deps` / `normalize_synthetic_learning_opinions` / `reset_flow_web_for_test` / `wait_flow_web_idle_for_test`。画面は w1-F の `native/web/flow/page.html` (`flow_html()`) を配信 |
| src/slack/slack-live.ts | native/src/slack/slack_live.{hpp,cpp} | `start_slack_flow` (同期) / `handle_slack_flow_reply` / `has_slack_sparring_session` / `reset_slack_live_for_test`。Web API は w1-D の `slack/web_api` (`SlackFlowDeps::webApi` で取得先を差し替え可)。`SlackFlowDeps::drivers` (テスト用のドライバ差し替え) を追加 |
| src/slack/slack-router.ts | native/src/slack/slack_router.{hpp,cpp} | `create_slack_router(SlackRouterConfig)` → `SlackRouter{onEvent, onInteractive}` (w1-D の `SocketModeDeps` の onEvent / onInteractive にそのまま結線できる)。議論の起動 (TS の `void startSlackFlow`) は `SlackRouterConfig::submit` (既定 `background_executor().submit`) に積む |

### 後続の担当 (main / gateway) 向けの使い方

- `/flow` の配線 (TS の index.ts 656 行目): `web::set_flow_web_deps({workspaceId, llm, openCore, listExternalVoices, sentimentClients, getYoutubeApiKey})`
  の後に `app.mount("/", web::flow_routes())`。本文上限は各ルートの `RouteOptions` に入っているので、HTTP サーバは
  `Router::max_body_for` を見ればよい。
- Slack (index.ts 864 行目): `auto r = slack::create_slack_router({config.slack.channelIds, deps, hooks})` →
  `slack::start_socket_mode({appToken, r.onEvent, r.onInteractive, ...})`。Socket Mode のハンドラは背景のプールで呼ばれ、
  議論の起動はさらに `submit` に積むので、ハンドラのスレッドを議論が塞がない。
- フォーラム入口: `flow::parse_forum_entry` / `parse_rounds_turns` / `parse_opponents` は純関数。`handle_forum_flow_post` は議論を最後まで
  進める同期 API なので Gateway からは背景のプールで呼ぶ。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| tests/flow/entrypoints.test.ts の parseForumEntry / handleForumFlowPost | flow.w5r_entry (golden entry.json + 同じ観点のケース) |
| (新規) parseRoundsTurns / parseOpponents の golden・境界 (1MiB の空白・繰り返し・壊れた UTF-8・桁あふれ) | flow.w5r_entry |
| tests/flow/entrypoints.test.ts の Web ルート部分 (400 の検査・410・say 404・UI・合成の声・議論一覧 (state / scope / ページング)・下書き再開・修正提案の適用・開始済みペーパー・声のシミュレーション・status のマーク・削除) | flow.w5r_routes (golden routes.json の 59 要求 + synthetic.json) |
| (新規) 固定フォーム・議論可能性チェック・メカニクス確認・ブロック改稿/適用・根拠クロール・戻す・機密の 400 の応答と LLM 呼び出し列・外部の声の検索呼び出し | flow.w5r_routes (golden) |
| (新規) 背景実行 (草案 → 下書き → 承認 → 議論 → done)、キュー満杯の 503、DELETE で進行中の議論を止める、本文上限 (ちょうど / +1 / 壊れた / 深すぎる / object 以外)、8000 文字の境界、巨大なクエリ | flow.w5r_routes |
| (TS テスト無し) information-gate-runner / flow-prep | flow.w5r_gate (golden gate.json / flow_prep.json + 機密・情報ゲート・旧 autoCrawl の分岐を偽の取得で) |
| (TS テスト無し) slack-live / slack-router | slack.w5r_slack (偽の Slack Web API) |

ctest 全体: 139/139 通過 (ビルドディレクトリ `/home/user/build-w5-R`、`ctest -j2`。このうち w5-R は flow.w5r_entry / flow.w5r_gate / flow.w5r_routes / slack.w5r_slack の 4 本)。

注: tests/flow/entrypoints.test.ts の Web 部分は現行の TS でも失敗する (壁打ちを `/api/flow/start` で起動して 200 を期待しているが、
`parseFlowKind` は学習・壁打ちのラベルを持たないので 400 になる。w4-P の port-notes の通り)。golden は現行の TS の挙動 (400) を記録し、
その後の観点 (一覧・下書き・削除など) は同じデータで golden に含めた。

## 3. 安全の対応表

| 防御・課金 | 場所 |
|---|---|
| loopback 信頼の Web UI (TS と同じ。認可は middleware/auth と HTTP bind の既定 loopback に従う) | flow_routes (main で mount) |
| 本文上限を各ルートに付けた (TS は無し): start 8MiB (仕様書の貼付本文を含む)・approve 4MiB・form/apply・block/apply・fix-suggestion/apply 2MiB・それ以外 256KiB。JSON は同じ大きさ + 要素 20 万 + 深さ 32 で読み、壊れた・上限超過・object 以外は TS の `.catch(() => ({}))` と同じく `{}` 扱い | routes.cpp `read_body` |
| LLM 応答の JSON (メカニクス確認の object) は 1MiB / 5 万要素 / 深さ 32、配列は `extract_json_array` (w2-G の上限)、合成の声の候補は 200 件まで (TS は全件) | routes.cpp |
| DB から読む値: status の行は 20 万行 / 256MiB、JSON 列 (top_utterance_ids_json / aufhebung_json) は 1MiB / 5 万要素 / 深さ 16 | routes.cpp `handle_status` |
| 機密 / 内部: 外部収集をしない (情報ゲート・旧 autoCrawl を省く)、承認で議論に渡す声・保存する声・議論可能性チェックの声は `gate_external_voices` を通す、根拠クロールは 400 | flow_prep.cpp, routes.cpp (golden conf-sess-1 で検索が一度も呼ばれないこと、テストで collector / LLM / Core が呼ばれないこと) |
| 仕様書 URL 欄 / ローカルパス: TS と同じく Web は `allowLocalPath=true` (loopback 信頼)。パス検査・大きさの上限・サイドカーの期限は w1-E の spec_source が課す。GitHub の token は gh から取り、github.com を含む URL にだけ渡す | routes.cpp `resolve_additional_spec_text` |
| Notion: w2-H の notion_link (Canalis サイドカー、深さ・ページ数・リンク数の上限つき) | routes.cpp |
| 利用者指定の URL (website のクロール) は crawler/sources/website の SSRF 対策のまま (allow_private_network=false) | learning_autocrawl (w3-K) |
| 背景実行の上限: 草案構築・議論・自動開始は `background_executor` (8 スレッド / キュー 256) に積み、満杯なら 503 (積まなかった仕事の状態は残さない、承認は承認前の状態に戻す)。タイマーのスレッドでは重い仕事をしない | routes.cpp `submit_background` / `start_approved_flow` |
| 並行: モジュールの状態 (編集エントリ・壁打ち・完了・声・進行中) は 1 つの mutex で守る。LLM の待ちは mutex の外。編集の適用 (版履歴・下書き行の同期) は mutex の中で直列 | routes.cpp |
| Slack: 設定チャンネル (channelIds) 以外・bot の投稿・message 以外は無視 (TS と同じ照合)。bot token はログ・エラー文に出さない (w1-D の web_api、テストでログに出ないことを確認)。保留表は 1000 件で古いものから捨てる (TS は無制限) | slack_router.cpp |
| Slack の個人データ: user 名 / ID は保存しない。テーマは投稿テキストのみ | slack_router.cpp |

lorica の課金を新たに入れた入力経路: Web の 17 ルートの本文 (`parse_json` の limits)、LLM 応答 (チェック系・合成の声)、status の DB 値。

## 4. 移していないもの・挙動の差

- 全ファイル移した (Node に残すものは無い)。
- **停止の配線**: Web 画面に「止める」操作は無い (page.html にも無い) ので、`DELETE /api/flow/:session` で進行中の議論の `FlowRunState::request_stop()`
  を呼ぶようにした (TS は削除しても背景の議論が発話を書き続けていた)。止まった議論は `FlowStopped` で終わり、finished に入る。
- **満杯の 503**: `/api/flow/start` (議論/改善) と `/paper/approve` は背景のプールが満杯なら 503 `{ok:false, error:"サーバが混み合っています…"}` (TS は常に受け付けた)。
- **本文が object 以外の JSON** (`null` 等) は `{}` 扱い (TS は `body.theme` で TypeError → 500)。
- `learningAppId` は整数だけを持つ (Number が NaN / 非有限なら無し、小数は切り捨て)。TS は NaN や小数をそのまま steam の collector に渡していた。golden はこの正規化をして比べた。
- `legacy_auto_crawl_before_flow` の `openCore()` の例外も握る (TS は try の外で、背景の Promise が reject していた)。
- 合成の声の類似度は TS と同じく UTF-16 のコード単位の bigram。`toLowerCase` は base の `str::to_lower` (ASCII・Latin-1・全角英字) で、それ以外の大文字小文字 (ギリシャ文字等) は変えない (w1 の規約と同じ差)。
- `extractJsonObject` (メカニクス確認の応答) は 1MiB を超えると読まない (TS は読んだ)。
- 学習・壁打ちの分岐 (Web の start / Slack の即起動) は TS と同じく残したが、`parse_flow_kind` が学習・壁打ちのラベルを持たないので到達しない (TS と同じ)。
- Web の壁打ちの `say` は同期で `submit_user` を呼ぶ (TS と同じく応答まで待つ)。
- Slack の保留表の上限 (1000) と、起動を積めないときは warn して起動しない (TS は常に起動)。

## 5. 土台に欲しいもの / 人間の判断が要ること

- 土台に欲しいもの
  - `background_executor` の満杯を作るテストで、プール全体を一時的に塞いでいる (他のテストと同じ実行ファイルなら影響する)。
    executor を差し替えられる口 (モジュールごとの executor 注入) が土台にあるとよい。
  - golden の PaperDraft の JSON で、固定欄つきのドラフトは TS では `issues` が固定欄の後ろに並ぶ (markdownToPaperDraft の spread の順)。
    `paper_markdown` の `to_json(PaperContent)` は issues を mechanics の直後に置くので、Web の応答 (`paper`) だけ routes 側で並べ直した。
    paper_markdown 側で順を TS に合わせると他の利用箇所も揃う (担当外なので触っていない)。
  - `str::to_lower` の Unicode 全域版 (JS の toLowerCase) があれば合成の声の類似度の差が消える。
- 人間の判断が要ること
  - DELETE で進行中の議論を止める配線 (C++ 版の追加) を受け入れてよいか。
  - 本文上限の値 (start 8MiB / approve 4MiB / 編集 2MiB / その他 256KiB) と 503 の文言。
  - tests/flow/entrypoints.test.ts の Web 部分 (壁打ちの起動を期待) が TS で失敗している件。テストを直すか、Web から壁打ちを起動できるようにするか。
