# port-notes w2-J — core の組み立て・KG レジストリ・診断の契約・起動時検査

job `cpp-node-20261030`。担当 TS 31 ファイルをすべて C++ に移した (Node に残したものは無い)。
golden 生成: `npx tsx native/tests/golden/w2-J-golden.gen.ts` (出力 `native/tests/fixtures/w2j/*.json`、値はすべて架空)。

## 1. 機能の対応表 (TS → C++)

| TS | C++ | 備考 |
|---|---|---|
| src/core/index.ts | native/src/core/index.{hpp,cpp} | `di::core::Core` / `create_core(path, events)`。`core.raw()` = TS の `core.client.raw` (`sql::Database&`)、`raw_ptr()` で共有ポインタ。graph は持たない (w1-C) |
| src/core/events/event-log.ts | core/events/event_log.* | `EventLog::append_event / load_events_since / rebuild_from_event_store` |
| src/core/repositories/base.ts | core/repositories/base.* | person/game/mechanic/aesthetic/affect/playContext/session/utterance/reaction/designGap/hypothesis。入力は TS の型どおりの struct |
| src/core/vectors/embedding.ts | core/vectors/embedding.* | |
| src/core/vectors/vector-search.ts | core/vectors/vector_search.* | `fetch_vectors_by_node_ids` / `search_similar` |
| src/core/vectors/query-embed-cache.ts | core/vectors/query_embed_cache.* | `warm_query_embedding` (background_executor、future を返す) と `_sync` 版 |
| src/core/kg-registry.ts | core/kg_registry.* | |
| src/db/repository.ts | db/repository.* | drizzle を生 SQL に。insert/update は TS のプロパティ名の JSON で受ける (列の型変換・既定値・`$defaultFn` は drizzle と同じ) |
| src/analysis/gap-rate.ts | analysis/gap_rate.* | |
| src/backup/runner.ts | backup/runner.* | 定期実行は `global_scheduler()` → `background_executor()`、archive は w1-A のサイドカー、S3 は w1-A の `backup/s3.hpp`。注入点 `BackupDeps` |
| src/crawler/sources/attribution-store.ts | crawler/sources/attribution_store.* | |
| src/crawler/sources/summarize.ts | crawler/sources/summarize.* | |
| src/kg-sync/pull.ts | kg_sync/pull.* | |
| src/fundamentum-export/store.ts | fundamentum_export/store.* | |
| src/design-diagnostic/documents.ts | design_diagnostic/documents.* | TS の `Map<string, DiagnosticReference>` は `DocumentMap` (挿入順・同 id は置換) |
| src/design-diagnostic/response.ts | design_diagnostic/response.* | |
| src/design-diagnostic/invoke.ts | design_diagnostic/invoke.* | |
| src/level-check/{play-model-validation,play-envelope,play-findings,request,prompt,response}.ts | level_check/*.{hpp,cpp} (同名) | |
| src/mechanics-check/{request,prompt,response}.ts | mechanics_check/*.{hpp,cpp} (同名) | |
| src/cernere-service-token/service-token-verify.ts | cernere_service_token/service_token_verify.* | w1-E の `paseto_v4_public` を使う |
| src/persona-bridge/bridge-auth.ts | persona_bridge/bridge_auth.* | |
| src/persona-bridge/assertion-replay-store.ts | persona_bridge/assertion_replay_store.* | |
| src/startup-guard.ts | startup_guard.* (`di::assert_startup_health`) | |
| src/discord-hook/forum-event-gate.ts | discord_hook/forum_event_gate.* | discord.js の `client.on` の代わりに `ForumEventGate::on_*` を Gateway 担当が呼ぶ |

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| tests/core/run.ts, tests/core/kg-registry.test.ts, tests/core/query-embed-cache.test.ts (warm/永続/本文非保存) | core.w2j_core (+ EventLog の行上限・ベクトルの壊れた行/巨大行/分割・キャッシュ行数上限・Fundamentum store) |
| tests/level-check/request.test.ts, play-envelope.test.ts, tool.test.ts の応答検証部分、tests/mechanics-check/request.test.ts, tool.test.ts の応答検証部分 | design_diagnostic.w2j_diagnostics (golden: 要求 40 件・応答 22 件×2・プロンプトは 1 バイト単位) + invoke の期限・同時実行・秘密を含めない |
| tests/crawler/external/attribution-store.test.ts、summary.test.ts の summarizeItems | crawler.w2j_sources (+ LLM 要約器の golden) |
| tests/flow/cernere-service-token.test.ts の verifyServiceToken と bridge route の認証部分、tests/flow/persona-bridge.test.ts の nonce 部分 | cernere_service_token.service_token_verify |
| tests/startup-guard/run.ts | startup_guard.startup_guard |
| tests/discord-hook/forum-event-gate.test.ts | discord_hook.forum_event_gate |
| (無し) gap-rate | analysis.gap_rate (golden: 架空の md と発話で Node と JSON・整形テキストが一致) |
| (無し) db/repository | db.repository |
| (無し) backup/runner | backup.runner |
| (無し) kg-sync/pull | kg_sync.pull (偽の master) |

担当外のモジュールが対象の部分 (tool.ts の checkLevelConsistency / checkMechanicsConsistency、importer、persona-bridge-routes) は移していない。

## 3. 安全の対応表

| 入力経路・防御 | C++ |
|---|---|
| service token (PASETO v4.public) | 判定順と理由は TS と同じ (aud 未設定 503 → 鍵が取れない 503 → 署名 401 → kind/sub/aud/exp/scope の型 401 → exp 401 → aud 401 → scope 403)。sub では分岐しない。境界テスト: 期限ちょうど (exp == now は期限切れ)・exp の不正/数値・型違いの claims・1 文字改ざん・別の鍵・空の鍵束・巨大 token・鍵ローダの例外/未設定。鍵はテスト内で OpenSSL EVP で生成 |
| persona bridge の Bearer | `/^Bearer ([^\s]+)$/` を線形の走査で (Node はヘッダを latin1 にするので \s は 09-0D/20/A0 の 1 バイト)。固定トークンは 32 文字 (UTF-16) 未満なら 503 default-deny、比較は `crypto::constant_time_equal`。31/32 文字の境界・長さ違い・未設定のテスト。warn には reason だけを書き、token を出さない (ログの sink で確認) |
| アサーションの再送防止 | 1 transaction で期限切れ削除 + INSERT OR IGNORE、1 行入ったときだけ true |
| startup guard | production で botToken 空は StartupHealthError、adminIds 空・mock backend は warn。メッセージに token を含めない。config に dev default の秘密値は無い (未設定は空で各経路が default-deny) ので拒否すべき既定値は無い |
| Di 管理フォーラムの受信境界 | `is_managed_forum_location` (w1-D) を使う。bot・reply・bot のリアクション・別スレッドを指す customId・newlyCreated でない ThreadCreate は捨てる。ManagedForumIds は provider から毎回読む (Ready 前は空 = 全拒否)。provider/fetch の例外は拒否 (fail closed)、ハンドラの例外は内容を出さずに warn |
| LLM 応答 (診断) | `responseBytes` (128KiB) を確かめてから `parse_json` (要素 10 万・深さ 64)。上限ちょうど/+1・50MiB・深い入れ子をテスト |
| 診断の LLM 呼び出し | 別スレッドで呼び timeoutMs で打ち切る。打ち切った呼び出しも数えて同時 4 本まで (超えたら llm_unavailable)。エラー文言にバックエンドの内容を含めない |
| kg-sync pull | http/https のみ・認証情報入り URL は拒否、期限 30 秒 (接続は期限以内)、応答本文は import の上限 (既定 256MiB)、リダイレクトを追わない (token を別の宛先へ渡さない)、エラー文言に URL・token・ホストを含めない (失敗の種類だけ)。上限ちょうど/-1 のテスト |
| events.jsonl | 1 行 8MiB (超えたら DB にも書かない)、全体 8GiB で追記停止 + warn。payload_json は上限つきで読み、行数・バイト数を課金 |
| vector_json | 1 行 4MiB・要素 20 万・深さ 4。全件走査は 200 万行・1GiB |
| repo の一覧 / attribution / gap-rate の読み出し | `sql::QueryLimits` で行数・バイト数を課金 |
| GameKG md (gap-rate) | 8MiB まで、slug は `fs::safe_join` で games_dir の下に限る (移植で追加)、KG は読み取り専用で開き作らない |
| backup の状態ファイル | 64KiB まで、壊れた/大きすぎる/型違いは空の状態。実行は 1 本ずつ (atomic)、重い仕事はスケジューラのスレッドでしない |
| クエリ埋め込みキャッシュ | 議題本文を保存しない (query_text は常に空、旧行は初回に消す)、行数 1000 (古い順に削除)、メモリ 4096 件で空にする (移植で追加)、同じクエリの多重発火は 1 本 |

## 4. 移していないもの・挙動の差

- Kuzu グラフ (EventLog.flushKuzu / rebuildKuzu、createCore の graph、backup の `discatier.kuzuGraphPath`) は持たない (w1-C と同じ理由。kuzuGraphPath は TS の config にも無い)。
- リポジトリの payload のキー順: TS は呼び出し側のオブジェクトリテラルの順。C++ は入力 struct の宣言順で、id は呼び出し側が渡したときは先頭、無いときは末尾。DB の値・射影は同じ。
- 診断の結果 (parse_*_response) の各所見で、TS は `{ ...finding, kind }` なので kind が末尾、C++ の `design_diagnostic::to_json(KindedFinding)` (w1-D) は先頭。テストはキー順を無視して比べる。プロンプト・引用照合用の playModel の JSON はキー順も一致。
- 応答 JSON の入れ子が 64 段を超えると TS は "expected object" 等の検証エラー、C++ は "expected JSON only" (どちらも invalid_response)。
- invoke_diagnostic の同時実行の上限 (4) は移植で追加。上限超過は `llm_unavailable` ("Diagnostic is busy")。
- service token の `exp` は `time::parse_iso` で読む (Date.parse より狭い。タイムゾーンなしの日時は UTC)。読めないものは TS の NaN と同じく expired。
- kg-sync pull: リダイレクトを追わない (TS の fetch は追う)。3xx は "KG pull failed: HTTP 302 Found"。通信失敗の文言は "KG pull failed: <timeout|network|...>"。
- db/repository: `countByMonitorId` は全行を取らず COUNT(*) (結果は同じ)。timestamp 列の入力はエポックミリ秒の数 (JS の Date の代わり) で、秒に切り捨てて保存。表に無いキーは drizzle と同じく無視、型の合わない値は std::invalid_argument。
- backup: 起動 30 秒後と 24h ごとの判定は同じ。scheduled の実行中に次の判定が来たら重ねない (TS は重なり得た)。
- gap-rate: 発話の取得で session id が 30,000 を超えると分割して問い合わせる (TS は SQLite の変数上限で失敗)。
- summarize: JS の slice がサロゲートペアを割る場合の孤立サロゲートは作らない (w1-D と同じ差)。
- forum event gate: discord.js の Message/Interaction の代わりに必要な部分だけの struct (`ForumMessageEvent` 等、元の payload は `raw`)。gateway.ts の配線 (TS テストの gateway.ts のソース照合) は Gateway 担当の範囲。
- query-embed-cache の warm は background_executor で動く。キューが満杯なら warn して温めない。

## 5. 土台に欲しいもの・人間の判断が要ること

- `sql::Database` に「閉じられたか」以外の世代番号があると、query-embed-cache の「接続ごとに 1 回だけ CREATE」の判定を weak_ptr に頼らずにできる。
- GCC 13 の `-Wdangling-reference` は `design_diagnostic::object()` 等「引数の参照を返す」補助に一時 std::string/vector を渡すと警告になる (今回は戻り値を束縛しない書き方で避けた)。validation.hpp に `[[gnu::no_dangling]]` 相当の注記か、ポインタを返さない API があると後続が書きやすい。
- 人間の判断: invoke_diagnostic の同時実行上限 4 の妥当性。打ち切った LLM 呼び出し (claude -p) を止める手段が LLMClient に無いので、裏で走り続ける。
- 人間の判断: events.jsonl の全体上限 (8GiB) と、超えたときに追記を止める (SoT の SQLite には書き続ける) 振る舞い。
