# port-notes w3-K — 学習・情報ゲート・ユーザーの声の収集・感想の受け口・経済グラフ・crawl チャンネル

job `cpp-node-20261030`。担当 TS 15 ファイルをすべて C++ に移した (Node に残したものは無い)。
ただし経済グラフの分析は「API キーで api.anthropic.com を直接呼ぶ経路」を移さず、LLMClient (claude -p) に置き換えた (§4)。
golden 生成: `tmp=$(mktemp -d) && cd "$tmp" && DATABASE_PATH="$tmp/x.db" /home/user/wt/w3-K/node_modules/.bin/tsx /home/user/wt/w3-K/native/tests/golden/w3-K-golden.gen.ts`
(`economy-analyzer.ts` が import 時に `data/discutere.db` を開くので一時ディレクトリで動かす)。
出力は `native/tests/fixtures/w3k/*.json` と `native/web/api/economy-graph.html` (値はすべて架空)。

## 1. 機能の対応表 (TS → C++)

| TS | C++ | 備考 |
|---|---|---|
| src/core/vectors/voice-index.ts | native/src/core/vectors/voice_index.{hpp,cpp} | `embed_voice_rows` / `embed_voices_by_ids` (`di::core::vectors`)。1 バッチ 1 トランザクション |
| src/crawler/sources/importer.ts | crawler/sources/importer.* | `import_external_utterances(core, items, opts)`。sidecar は生ポインタ (所有しない) |
| src/flow/density-blackbox.ts | flow/density_blackbox.* | `lib/blackbox` (w1-B) の `make_sqlite_blackbox` / `engine->decide` |
| src/flow/information-gate.ts | flow/information_gate.* | `evaluate_information_density` / `run_information_gate`。LLM は `persona_engine::LLMClient*` |
| src/flow/learning-autocrawl.ts | flow/learning_autocrawl.* | `default_collectors()` (niconico / youtube / steam / website / reddit は w1-D の crawler/sources) |
| src/flow/learning.ts | flow/learning.* | `run_learning_flow`。感情カスケードは lib/sentiment_core、md は flow/games_md |
| src/flow/user-voices/game-voices.ts | flow/user_voices/game_voices.* | `list_game_voices` |
| src/flow/user-voices/collect.ts | flow/user_voices/collect.* | `collect_game_voices` / `describe_voice_report` / `glab_game_slug` 等 |
| src/flow/user-voices/runtime.ts | flow/user_voices/runtime.* | `prepare_user_voices` / `default_collect_deps` / `theme_game_ref` / `parse_similar_games`。`embedding_config_of(cfg)` を追加 |
| src/persona-bridge/impression-intake.ts | persona_bridge/impression_intake.* | `parse_impression_batch` / `ingest_impressions` |
| src/api/impression-bridge-routes.ts | api/impression_bridge_routes.* | `create_impression_bridge_routes(opts)` ("/api" に mount)。認証は w2-J の `persona_bridge/bridge_auth` |
| src/ludus/economy-analyzer.ts | ludus/economy_analyzer.* | `analyze_economy(ws, title, deps, force)` / `analyze_economy_in_background` / `to_slug` / `extract_json` |
| src/api/economy-graph-routes.ts | api/economy_graph_routes.* + native/web/api/economy-graph.html | ("/" に mount)。画面は Node の出力をそのまま置き、`{{slugJson}}` / `{{apiBaseJson}}` を差し込む |
| src/discord-hook/crawl-channel.ts | discord_hook/crawl_channel.* | 純関数 + `ingest_crawl_urls(deps, input)` |
| src/discord-hook/crawl-handler.ts | discord_hook/crawl_handler.* | `handle_crawl_message(DiscordApi&, const Message&, deps)`。Gateway の担当が MESSAGE_CREATE から呼ぶ (普通の関数) |

配線 (main.cpp / Gateway / index.ts の `triggerEconomyAnalysis` 等) は担当外。後の担当は次を渡す:
- `api::create_economy_graph_routes({workspace, AnalyzeEconomyDeps{ClaudeCliClient, nullptr, {}}})` を "/" に mount。
  Discord の経済分析ボタン (TS の `triggerEconomyAnalysis`) は `ludus::analyze_economy_in_background(ws, title, deps)` + `to_slug(title)`。
- `api::create_impression_bridge_routes()` を "/api" に mount (既定は env と active KG)。
- `discord_hook::handle_crawl_message` は数十秒かかるので `background_executor()` から呼ぶ。`CrawlDeps.reddit` は
  `flow::reddit_credentials_from_env()`、`summarizer` は `crawler::sources::create_llm_summarizer`。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| tests/flow/information-gate.test.ts | flow.w3k_gate (+ プロンプトと応答の解釈の golden 40 件、LLM 応答の上限ちょうど/+1/巨大/深い入れ子/件数上限、失敗の継続、不正な minDensity) |
| tests/flow/density-blackbox.test.ts | flow.w3k_gate (+ 特徴量の golden、ルール output 不正時の sparse、深すぎる条件の拒否) |
| tests/flow/learning-autocrawl.test.ts | flow.w3k_gate (+ 既定 collector の入力検査、website の私設アドレス拒否、slug の golden) |
| tests/flow/learning.test.ts | flow.w3k_gate (investigate が読める・LLM を呼ばない・個人データ非保管・日本語 slug・自動収集・1 ソース失敗) |
| tests/flow/user-voices.test.ts の collect / listGameVoices / parseSimilarGames 部分 | flow.w3k_user_voices (+ theme_game_ref / describe_voice_report の golden、quoted_terms の境界) |
| tests/flow/impression-bridge.test.ts の「届いていた感想への fallback」 | flow.w3k_user_voices |
| tests/crawler/external/importer.test.ts、summary.test.ts の importer 部分 | flow.w3k_user_voices |
| (無し) voice-index / prepare_user_voices | flow.w3k_user_voices |
| tests/flow/impression-bridge.test.ts の検査・取込・受け口 | persona_bridge.w3k_impression (+ 検査の golden 13 件、トークン 31/32 文字、内部例外を出さない、JSON の要素数・深さ・件数、本文上限ちょうど/+1/巨大を実サーバで) |
| tests/discord-hook/crawl-channel.test.ts | discord_hook.w3k_crawl (+ golden、巨大な本文・slug 長・appId 桁・壊れた UTF-8、偽の取得での取り込み、SSRF、偽 Discord でのリアクション・通知・返信フォールバック) |
| (無し) economy-analyzer / economy-graph-routes | api.w3k_economy (画面の golden、LLM 呼び出しの引数・プロンプト、整形・保存・キャッシュ、失敗・巨大応答、ルート、多重起動) |

tests/flow/user-voices.test.ts の steam-app / voluptas-impressions 部分は w1-D (crawler.fetch / crawler.sources)。

## 3. 安全の対応表

| 入力経路・防御 | C++ |
|---|---|
| 感想の受け口の認証 | w2-J の `authorize_bridge_request` (service token / 固定トークン 32 文字以上・定数時間比較・未設定 503)。判定は本文を読む前 (TS と同じ順) |
| 感想の匿名性 | 6 項目以外のキーを持つ感想は 1 件ごと拒否 (`non_anonymous_field`)。書き手は常に `anonymous` で保存 (w1-D の `map_impression`) |
| 感想の形と上限 | id `/^[\w.:-]{1,128}$/`、gameTitle 200・comment 10,000 (UTF-16)、createdAt は日付として読める、1 回 200 件 (TS と同じ)。**本文 8MiB (413)・JSON 要素 1 万・深さ 8 (400) を追加**。応答はすべて `cache-control: private, no-store`、取込の内部例外は 500 で中身を応答にもログにも出さない |
| 情報ゲートの LLM 応答 | `extract_json_object` (1MiB・要素 5 万・深さ 32) で読み、壊れていれば件数ベースの fallback (8 件 rich / 3 件 moderate)。**covered 100 件・gaps 50 件の上限を追加**。LLM 例外・失敗・未設定も fallback |
| 密度ルールの提案 (LLM 生成の条件) | w1-B の `validate_condition` (8 段まで) を通らなければ候補にしない。ルールの output が不正なら sparse (学習継続) |
| 経済グラフの LLM 応答 | 1MiB を超えたら失敗、JSON は要素 10 万・深さ 32。nodes / edges は object だけ・summary は文字列だけ・economyLoops は文字列だけ残す (TS は検証せず保存していた) |
| 経済グラフのキャッシュ (DB の値) | 4MiB・要素 20 万・深さ 32 で読む。壊れていれば 500 / 失敗 |
| 経済グラフの画面 (XSS) | slug (URL 由来) と Host ヘッダ由来の apiBase を `<script>` に差し込むとき `< > & U+2028 U+2029` を `\u` エスケープ (TS の `JSON.stringify` のままだと `</script>` で抜けられた)。単一走査で置き換え、値の中の `{{…}}` は再走査しない |
| 経済グラフの POST | 本文 16KiB・JSON 要素 1000・深さ 16、gameTitle 500 文字 (UTF-16) を超えたら 400 (**追加**)。同じゲームの分析は同時に 1 本 (**追加**)。背景キューが満杯なら warn して受け付けない |
| 経済グラフの API キー | **API キーで api.anthropic.com を直接呼ぶ経路を持たない** (LLMClient = claude -p のみ)。DB の LIKE 検索は TS と同じく行数 20・8MiB の上限つき |
| crawl チャンネル | website (利用者が貼った URL) は w1-D の `fetch_website_articles` (allow_private_network=false、解決 IP 検査) のまま。1 URL あたりの上限 (steam/youtube/reddit 500、website 1) は同じ。**1 メッセージ 20 URL まで・まとめは 2000 文字で切る・まとめの投稿は一括メンションを解釈させない (`allowed_mentions.parse=[]`)** を追加 |
| crawl の URL 解析 | 正規表現を使わない線形の走査 (巨大な本文・壊れた UTF-8 で確認)。**19 桁以上の Steam appId は website 扱い** (整数で持てないため) |
| 学習クロール | website 既定 collector は私設アドレスを拒否 (テストで loopback / リンクローカルのメタデータアドレス / 私設 10 系)。クロール失敗は warn して議論を止めない |
| 部分ベクトル化 | 埋め込みの入力は 2000 (UTF-16) で切る。**1 回 2 万 id まで** (追加)。埋め込みの返したベクトルが足りなければ失敗 (TS は undefined で登録に失敗) |
| importer | **1 回 10 万件まで** (超えた分は skipped に数える。追加)。postedAt が NaN の項目は末尾に並べる |
| 機密タグ・仮名化 | 本担当は外部の声を露出しない (露出の仮名化・`gateExternalVoices` は呼び出し側 = 先行担当)。speaker_id は TS と同じ `ext:<source>:<authorId>`、学習フローの意見は `anon` 固定 |

## 4. 移していないもの・挙動の差

- **経済グラフの分析 (人間の判断が要ること §5 も参照)**: TS は Anthropic API の tool use (最大 4 ターン) で LLM に
  `query_guide_mechanics` / `search_discussion_knowledge` を呼ばせていた。C++ は API キーを持たず `claude -p` (LLMClient) を 1 回呼ぶ。
  ツールの代わりに同じ問い合わせを先に実行してプロンプトに載せる: 攻略メカニクス (ゲーム名で 30 件) + 議論知識
  (ゲーム名で 10 件 + 攻略メカニクス上位 5 語で各 5 件)。システムプロンプトのツールの説明だけを「集めた情報の説明」に替えた
  (粒度・辺の種類・出力形式の指示は同じ)。LLM が自分で追加検索することはできない。出力 JSON・キャッシュ表・HTTP 経路・画面は同じ。
  モデル指定は TS と同じ `claude-haiku-4-5-20251001` を claude -p に渡す。「ANTHROPIC_API_KEY not set」は「LLM not configured」になる。
- 経済グラフの graph は object 以外の node / edge、文字列以外の summary / economyLoops を捨てる (TS はそのまま保存して画面が壊れ得た)。
- 経済グラフの POST の `gameTitle` は文字列だけ受ける (TS は truthy なら何でも通した)。`force` は JS の truthy と同じ判定。
- 感想の受け口: 8MiB を超える本文は認証より前に 413 (ルータの本文上限)。その 413 には cache-control が付かない。
  JSON の要素数・深さの上限超過は "body must be JSON" の 400。
- `glab_game_slug` / `normalize` 系は w1-F の NFKC・小文字化の近似を使う (ASCII と日本語は golden で一致。範囲外の差は port-notes/w1-F.md)。
- crawl の URL 解析は WHATWG URL ではなく `base/url` の `url::parse`: 非 ASCII のホスト名 (punycode 化されない)・非 ASCII のパス (percent-encode されない)
  では slug / videoId が TS と異なり得る。http/https 以外は website 扱い (抽出される URL は http/https だけなので実害なし)。
- crawl のまとめを元チャンネルへ返信するとき、TS (discord.js の reply) と同じく返信先には通知するが、本文中の @everyone 等は解釈させない。
- `CrawlDeps` / `PrepareUserVoicesArgs` / `AnalyzeEconomyDeps` にテスト用の差し替え口 (取得・sidecar・KG・設定・埋め込み) を足した。既定は TS と同じ。
- `GameVoiceReport.voluptasDown` は TS と同じく「Voluptas 取得が例外で失敗したときだけ」キーが出る (optional<bool>)。
- importer の postedAt NaN の並べ替え: JS の sort は NaN の比較が不定。C++ は安定ソートで NaN を末尾に回す。
- 情報ゲートの `minDensity` が不正なときは TS と同じく学習せず `sufficient=false`。
- 学習フローの polarityBreakdown・crawledBySource は TS と同じキー順の JSON object。

## 5. 土台に欲しいもの・人間の判断が要ること

- **人間の判断: 経済グラフの分析を claude -p の 1 回呼び出し (DB の情報を先に載せる) にしたこと**。tool use の多段検索 (LLM が
  語を選んで追加検索する) は無くなった。結果の質が足りなければ、(a) 事前検索の語を増やす、(b) claude -p の MCP ツールとして
  読み取り専用の DB 検索を渡す、のどちらかを決める必要がある。API キーで直接呼ぶ経路は指示書どおり持たない。
- 人間の判断: 追加した上限 (感想の本文 8MiB、crawl 1 メッセージ 20 URL、経済グラフの gameTitle 500 文字、importer 10 万件) の妥当性。
- 土台: `http::Router` の本文上限で返す 413 に、ルートごとの既定ヘッダ (cache-control 等) を付ける手段があると、no-store を全応答に揃えられる。
- 土台: `base/url` に WHATWG 相当のホスト正規化 (IDN → punycode) があると、crawl の slug を TS と完全に揃えられる。
- ディスク: このセッションの作業環境はディスクが逼迫している (共有 252G 中、空き 1GB 前後)。ビルドは `-O0` (デバッグ情報なし) と
  `-s` (strip) で構成した (`CMAKE_CXX_FLAGS_DEBUG=-O0`, `CMAKE_EXE_LINKER_FLAGS=-s`。CMake ファイルは変更していない)。
