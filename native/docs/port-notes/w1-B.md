# port-notes w1-B — persona-engine の基礎と LLM ライブラリ

job `cpp-node-20261030`。担当: persona-engine の型・DB・プロンプト・seed・LLM クライアント (codex / ローカル OpenAI 互換 / mock)・
ワーカー起動、discussion (編成・進行)、`@ludiars/llm-gateway`、`@ludiars/blackbox`。

## 1. 機能の対応表 (TS → C++)

| TS | C++ | 名前空間 |
|---|---|---|
| src/persona-engine/types.ts | native/src/persona_engine/types.{hpp,cpp} (TokenUsage は llm/client.hpp を include して使う) | di::persona_engine |
| src/persona-engine/context-provider.ts | persona_engine/context_provider.{hpp,cpp} (純粋仮想クラス。任意メソッドは既定実装 = 空/nullopt) | di::persona_engine |
| src/persona-engine/db/migrations.ts | persona_engine/db/migrations.{hpp,cpp} (DDL は TS と同じ文) | di::persona_engine::db |
| src/persona-engine/db/personas-repo.ts | persona_engine/db/personas_repo.{hpp,cpp} | 同上 |
| src/persona-engine/db/rules-repo.ts | persona_engine/db/rules_repo.{hpp,cpp} | 同上 |
| src/persona-engine/db/session-fires-repo.ts | persona_engine/db/session_fires_repo.{hpp,cpp} | 同上 |
| src/persona-engine/engine/prompt-builder.ts | persona_engine/engine/prompt_builder.{hpp,cpp} | di::persona_engine::engine |
| src/persona-engine/facilitator/prompts.ts | persona_engine/facilitator/prompts.{hpp,cpp} | di::persona_engine::facilitator |
| src/persona-engine/llm/pricing.ts | persona_engine/llm/pricing.{hpp,cpp} | di::persona_engine (既存の llm/*.hpp に合わせた) |
| src/persona-engine/llm/tier-routing.ts | persona_engine/llm/tier_routing.{hpp,cpp} | 同上 |
| src/persona-engine/llm/codex-cli.ts | persona_engine/llm/codex_cli.{hpp,cpp} | 同上 |
| src/persona-engine/llm/local-openai.ts | persona_engine/llm/local_openai.{hpp,cpp} | 同上 |
| src/persona-engine/llm/mock.ts | persona_engine/llm/mock.{hpp,cpp} | 同上 |
| src/persona-engine/seeds/personas.ts | persona_engine/seeds/personas.{hpp,cpp} | di::persona_engine::seeds |
| src/persona-engine/seeds/rules.ts | persona_engine/seeds/rules.{hpp,cpp} | 同上 |
| src/persona-engine/worker-pool/debate-rules.ts | persona_engine/worker_pool/debate_rules.{hpp,cpp} | di::persona_engine::worker_pool |
| src/persona-engine/worker-pool/persona-prompts.ts | persona_engine/worker_pool/persona_prompts.{hpp,cpp} | 同上 |
| src/persona-engine/worker-pool/spawner.ts | persona_engine/worker_pool/spawner.{hpp,cpp} | 同上 |
| src/persona-engine/worker-pool/types.ts | persona_engine/worker_pool/types.hpp (settleTimer は Scheduler::Id) | 同上 |
| src/discussion/composition.ts | native/src/discussion/composition.{hpp,cpp} | di::discussion |
| src/discussion/director.ts | discussion/director.{hpp,cpp} (DirectorLLM は純粋仮想、投稿・続行の問い・ログは std::function で受ける) | di::discussion |
| lib/lapilli/packages/llm-gateway/src/* | native/src/lib/llm_gateway/{types,tokens,routing,ordering,summary,llm_gateway} | di::lib::llm_gateway |
| lib/lapilli/packages/blackbox/src/* | native/src/lib/blackbox/{types,condition,fingerprint,lifecycle,stats,engine,store_memory,store_sqlite,store_jsonfile,blackbox} | di::lib::blackbox |

discussion/director.ts は discord-hook 等に依存していない (依存はすべて DirectorDeps の注入)。director-live.ts (Discord 結線) は担当外。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| lib/lapilli/packages/llm-gateway/src/index.test.ts | lib.llm_gateway (+ golden `w1-B-llm_gateway.gen.ts`) |
| lib/lapilli/packages/blackbox/src/engine.test.ts | lib.blackbox_engine |
| lib/lapilli/packages/blackbox/src/fingerprint.test.ts | lib.blackbox_fingerprint (+ golden `w1-B-blackbox.gen.ts`: canonicalJson / validate / describe / evaluate) |
| lib/lapilli/packages/blackbox/src/store-sqlite.test.ts | lib.blackbox_store_sqlite |
| lib/lapilli/packages/blackbox/src/store-jsonfile.test.ts | lib.blackbox_store_jsonfile |
| tests/persona-engine/repos.test.ts, session-fire-store.test.ts | persona_engine.repos |
| tests/persona-engine/local-openai.test.ts (+ codex-cli / mock) | persona_engine.llm_clients (偽の HTTP サーバ・偽の codex) |
| tests/persona-engine/facilitator.test.ts の extractJsonObject 部分 + プロンプト | persona_engine.prompts_golden (golden `w1-B-persona_engine.gen.ts`) |
| (spawner は TS テスト無し) | persona_engine.spawner (偽の lictor を引数配列で起動・kill) |
| tests/discussion/composition.test.ts | discussion.composition (+ golden `w1-B-discussion.gen.ts`) |
| tests/discussion/director.test.ts | discussion.director (+ golden: プロンプト・投稿・ログ・失敗ターン) |

facilitator.test.ts の残り (createFacilitator / evaluate) と engine / adapter 系テストは担当外モジュールのもの。

## 3. 安全の対応表

| 経路 | 防御 |
|---|---|
| local-openai の HTTP 応答 | `max_response_bytes` 8 MiB、parse_json (8 MiB / 20 万要素 / 深さ 64)、期限 (timeoutMs、接続は min(timeout,10s))。設定済みローカルなので allow_private_network=true。apiKey はログに出さない |
| codex exec | `one_shot::run` の引数配列 (シェル不使用、API キー系 env 遮断) + 許可リスト env、期限、stdout 32 MiB / stderr 4 MiB で打ち切り、`-o` ファイルは 1 MiB 上限 (超過は読まない)、一時ディレクトリは 0700 で作り必ず消す。stderr・ローカルパスは上位へ出さない |
| ワーカー起動 (spawner) | シェル不使用 (TS の Windows `cmd.exe /c lictor` は廃止)、引数配列、親の Lictor/Concordia 連携 env を剥がす、stdio は /dev/null、終了は背景で回収 |
| LLM 応答 JSON (facilitator extract_json_object) | 1 MiB / 5 万要素 / 深さ 32 |
| DB 値: personas.traits | 256 KiB / 1 万要素で読む (壊れていれば [] = TS と同じ) |
| DB 値: personas.learned_notes | 4 MiB / 20 万要素。壊れた JSON は [] (TS と同じ)、上限超過は上書きで消さないよう例外 |
| DB 値: blackbox の JSON 列 | 列ごと 1 MiB / 10 万要素 / 深さ 64、壊れていれば例外 (TS の JSON.parse と同じく信頼しない) |
| blackbox JSON ファイル | 64 MiB 上限。超過は例外 (下記 4)、壊れた JSON は空状態 (TS と同じ) |
| blackbox 条件 | validate は深さ 8 (TS と同じ)、evaluate / describe は深さ 64 で打ち切り (保存値の再帰で落ちない)、canonical_json は深さ 256 |
| prompt-builder の語分割 | std::regex を使わず線形走査 (巨大な議題文でも線形) |

## 4. 移していないもの・挙動の差

- **src/persona-engine/llm/anthropic.ts / claude-code-auth.ts は移していない**: API キー / Claude Code の OAuth トークンで
  api.anthropic.com を直接呼ぶ経路で、指示書の「LLM の呼び出しでは API キーを持たない (claude -p の CLI を使う)」に反する。
  C++ では backend=anthropic の設定も `claude -p` (ClaudeCliClient) にフォールバックさせる想定 (config / dispatch の担当が結線)。
- director: TS は LLM が失敗し続けると内側のループが永久に回る。C++ は連続 20 回 (`kMaxConsecutiveSkips`) 失敗・空応答で
  そのラウンドを打ち切って続行の問いに進む (暴走ガード)。それ以外の進行・プロンプト・ログは golden で一致。
- spawner (Windows): `cmd.exe /c lictor` を使わず、PATH 上の `lictor.exe` を CreateProcessW (DETACHED_PROCESS) で直接起動する。
  lictor が npm の `.cmd` shim しか無い環境では起動できない (warn ログ、pid=nullopt)。kill は `taskkill /PID <pid> /T /F` を引数配列で。
- codex-cli: Node の errno 付きエラー (`(EACCES)` 等) は一時領域作成では付けない。実行ファイルが無いときは `codex cli error (ENOENT)`、
  シグナル終了は `codex cli exit null` (TS と同じ表記)。stdout が 32 MiB を超えたら失敗 (`codex cli: output exceeds limit`、TS は捨て続ける)。
- local-openai: 応答 JSON が壊れているときのエラー文は Node の SyntaxError 文言ではなく parse_json の理由になる (`local LLM invoke failed: ...` の接頭は同じ)。
- ContextGap.description は JSON に常に出す (値が無ければ null)。本番の adapter は常に string|null を入れるので同じ形。
- blackbox: Condition / FeatureMap / output は di::Json で持つ (保存→読み戻しでキー順を保つ)。JSON ファイルの上限超過は例外 (TS は空状態にして次の保存で上書き = データが消える)。
  JSON ファイル store は同じファイルの load→save を mutex で直列化 (TS は単一スレッド前提)。LlmFallback は同期関数。
- tokens: 不正な UTF-8 は U+FFFD 1 文字として数える (JS の文字列には無い入力)。ordering: 未知の stability は末尾に寄せる (TS は NaN 比較で不定)。
- composition: rng が [0,1) 外の値を返しても添字を範囲内に収める (TS は undefined → 先頭名)。

## 5. 土台に欲しいもの・人間の判断が要ること

- 土台に欲しいもの: 切り離した常駐プロセスの起動 (`proc::spawn_detached`: setsid / DETACHED_PROCESS、stdio 捨て、回収) と
  プロセスグループ kill。今は worker_pool/spawner.cpp に局所実装した。parse_json の失敗理由を種別 (上限超過 / 構文) で返す API
  (今は理由の文字列で判定している)。
- 人間の判断: Windows で lictor を `.cmd` shim 経由でしか起動できない環境をどう扱うか (lictor.exe を配る / node + JS エントリの
  既知パスを決める)。anthropic backend を設定している運用者への案内 (claude -p への切替)。director の暴走ガード値 (20) の妥当性。
