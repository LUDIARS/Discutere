# port-notes w3-L: 議論の進行 (rounds エンジン)・結論・結論の可視化と export

job `cpp-node-20261030`。担当の TS 13 ファイル (約 2,360 行) を C++ に移した。
golden 生成: `npx tsx native/tests/golden/w3-L-flow.gen.ts` → `native/tests/fixtures/flow/w3l_golden.json`。

director の golden は「偽の LLM に Node と同じ応答を与え、同じものができるか」を見る:
Node 側はプロンプトの種類 (価値軸・開幕・発話・投票・要約・止揚判定・結論・リファイン) で決まった応答を返す偽の LLM で
`runFlow` を最後まで回し、LLM への呼び出し列 (system / prompt / model / maxTokens) と応答、DB に残る全表の全行
(時刻・latency の列を除く)、ペーパー md (`discussion_paper.body_md`)、onUtterance / onVote の通知、log / warn、戻り値を記録する。
C++ 側は記録した応答を順に返し (`ReplayLLM`)、同じ呼び出し列・同じ行・同じ通知になることを確かめる。
randomUUID は出てきた順に `<U1>` `<U2>` … に置き換えて比べ、応答に含まれる `<Uk>` は C++ 側の実 id に戻して返す
(ペルソナ価値軸の応答が personaId を含むため)。乱数は両側同じ LCG を注入する。
投票者は 1 人にしている: 同票のくじは候補 id (乱数の UUID) の昇順で引くので、同票があると Node と C++ で勝者が変わる
(実装の差ではなく id が乱数であるため。同票のくじ自体は w2-G の vote の golden で確認済み)。
シナリオは 3 本: basic (投票・止揚・JSON 結論・空応答のスキップ・外部の声)、override (確定ペーパー・承認済み論点・
機密タグの想定意見・開幕と発話の LLM エラー・早期収束・結論の JSON 失敗 2 回 → 未検証・議論適性の注記・リファイン)、
scores (improvement フロー・機械スコアの評価差し替え・onVote の例外・憑依・結論 LLM 失敗)。

## 1. 機能の対応表 (TS → C++)

| TS | C++ |
|---|---|
| src/flow/director.ts | native/src/flow/director.{hpp,cpp} (`FlowUtteranceRecord` / `VoteEvent` / `FlowRunOptions` / `run_flow` / `run_discussion_flow` / `persist_utterance` / `clamp_count` / `is_vote_candidate`、追加の `FlowRunState` / `FlowStopped`) |
| src/flow/persona-turn.ts | native/src/flow/persona_turn.{hpp,cpp} (`run_persona_turn`) |
| src/flow/facilitator-turn.ts | native/src/flow/facilitator_turn.{hpp,cpp} (`build_facilitator_prompt` / `run_facilitator_turn`) |
| src/flow/conclusion.ts | native/src/flow/conclusion.{hpp,cpp} (`generate_conclusion` / `build_rounds_section` / `UNVERIFIED_PREFIX`) |
| src/visualize/conclusions.ts | native/src/visualize/conclusions.{hpp,cpp} (`ConclusionSummary` / `ConclusionDetail` の型と JSON、`list_conclusions` / `get_conclusion_detail`) |
| src/visualize/flow-conclusions.ts | native/src/visualize/flow_conclusions.{hpp,cpp} |
| src/visualize/conclusion-cache.ts | native/src/visualize/conclusion_cache.{hpp,cpp} |
| src/visualize/conclusion-markdown.ts | native/src/visualize/conclusion_markdown.{hpp,cpp} (`lib/front_matter::stringify`) |
| src/visualize/learning-cache.ts | native/src/visualize/learning_cache.{hpp,cpp} (`LearningCacheReader` クラス) |
| src/fundamentum-export/snapshot.ts | native/src/fundamentum_export/snapshot.{hpp,cpp} |
| src/fundamentum-export/list.ts | native/src/fundamentum_export/list.{hpp,cpp} |
| src/fundamentum-export/export.ts | native/src/fundamentum_export/export.{hpp,cpp} |
| src/api/conclusion-publication-routes.ts | native/src/api/conclusion_publication_routes.{hpp,cpp} (`create_conclusion_publication_routes`、`request_url_parts` / `bearer_token_of` を公開) |

使い方の規約は w2-G と同じ (LLM を呼ぶ関数は第 1 引数に `LLMClient&`、残りを Args struct)。
`run_flow` の options は TS の `FlowRunOptions` のフィールド名のまま。`evaluateRound` は同期の `std::function<VoteResult(const RoundEvalContext&)>`
(`RoundEvalContext::llm` はポインタ)。`onUtterance` / `onVote` も同期のコールバック。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| tests/flow/discussion.test.ts の FlowDirector 統合 (rounds × turns の発話永続・ペーパー永続・LLM 失敗のエラー発話と llm_call_log) | flow.w3l_director |
| tests/flow/possession-vote.test.ts (憑依の casual 名保持・投票通知の候補) | flow.w3l_director |
| tests/flow/turn-scheduler.test.ts の buildFacilitatorPrompt と director 部分 | flow.w3l_director (golden の facilitatorPrompts + basic/override の呼び出し列) |
| tests/flow/conclusion-convergence.test.ts / vote-conclude.test.ts の結論部分 (JSON リトライ・未検証・早期収束・注記) | flow.w3l_director (golden override/scores + conclusion の単体) |
| tests/flow/paper-review.test.ts の director 部分 (paperOverride が investigate を上書きして永続) / persona-state.test.ts の director 部分 | flow.w3l_director (golden override: discussion_paper / flow_session_persona の行) |
| tests/conclusion-cache/conclusion-cache.test.ts (topic-terms を除く) | visualize.w3l_conclusions |
| tests/visualize/flow-conclusions.test.ts | visualize.w3l_conclusions (golden flowConclusions) |
| tests/visualize/conclusion-markdown.test.ts | visualize.w3l_conclusions (golden markdown は 1 バイト一致) |
| (無し) conclusions.ts の KG 経路・learning-cache | visualize.w3l_conclusions (golden kgConclusions + 焼いて読み戻す) |
| tests/fundamentum-export/run.ts | fundamentum_export.w3l_export (+ snapshot と content id の golden) |
| tests/visualize/conclusion-publication.test.ts のルート部分 | api.conclusion_publication_routes |

ctest 全体: 108/108 通過 (ビルドディレクトリ `/home/user/build-w3-L`、このうち w3-L の 4 本: flow.w3l_director /
visualize.w3l_conclusions / fundamentum_export.w3l_export / api.conclusion_publication_routes)。

## 3. 安全の対応表

| 防御 / 入力経路 | C++ |
|---|---|
| GLAB 結論 API のトークン | TS と同じ: 32 バイト (UTF-8 の長さ) 未満・未設定は 503 `conclusions_unconfigured`、`^Bearer (\S+)$` で取り出し、長さが違えば 401、同じなら `crypto::constant_time_equal`。Node と同じくヘッダ値を latin1 として読み UTF-8 にしてから比べる (`\S` も latin1 の範囲の JS 空白 = 0x09–0x0D, 0x20, 0xA0)。クライアントの role / audience ヘッダは見ない。31 / 32 / 33 バイト・1 文字違い・空白混入・64KiB の token でテスト |
| 公開設定のローカル操作 | TS と同じ: `middleware::is_loopback_request` (相手の生 IP) かつ要求 URL のホスト名が localhost / 127.0.0.1 / [::1]、Origin があれば要求の origin と完全一致。URL は @hono/node-server と同じく Host ヘッダ (絶対形式の要求行ならその URL) から組み、既定ポートは origin から省く。Host が無い・読めない (ポートが数字でない・65535 超・userinfo・空白) は 403 |
| PUT 本文 | `req.json` を 1MiB / 1 万要素 / 深さ 16 で読む。キーの許可リスト・型・scope の許可リスト・id は 1〜500 (UTF-16 単位、TS の `.length`)。content-type は `application/json` で始まること (大文字小文字を区別、TS と同じ) |
| 全応答 | `Cache-Control: private, no-store` |
| DB の JSON 列 (aufhebung_json / top_utterance_ids_json / tags_json / mechanics_json) | `parse_json` を 4MiB / 20 万要素 / 深さ 32 で読む。上限超過・壊れていれば空扱い (TS の try/catch と同じ結果)。上限ちょうど / +1 / 壊れた / 深さ 1 万でテスト |
| DB の行 | 一覧・発話・止揚は `sql::QueryLimits` (20 万行 / 256MiB、キャッシュ一覧は 100 万行 / 512MiB) |
| 学習キャッシュの payload | TS は `JSON.parse` のまま読んでいた。C++ は 64MiB / 400 万要素 / 深さ 64 で読み、壊れていれば無いものとして既定値を返す |
| LLM の応答 JSON (結論) | `facilitator::extract_json_object` (`llm_json_limits`)。5MiB の応答・壊れた JSON は未検証の生テキストに落ちる (テスト済み) |
| 個人アンカーの仮名化 | conclusions の話者ラベルは TS と同じ (persona は表示名、`ext:` は `crawler::sources::masked_persona_label` の 論者#xxxxxx、それ以外は「人間」、無しは「参加者」)。flow_utterance は内部ペルソナのみで出所 null |
| 機密タグ | `機密` のときユーザ意見の代わりに synthetic の想定意見を配る (persona_turn → build_persona_paper、TS と同じ。golden override で確認) |
| content-addressed の dedup | snapshot に時刻等の可変値を入れない (TS と同じキーのみ)。同じ detail → 同じ content id を golden で Node と照合 |
| 外部 I/O の期限 / 同時実行 | director は TS と同じく逐次 (並行の呼び出しは無い)。LLM の期限は各 LLMClient の既定。HTTP は持たない |

## 4. 移していないもの・挙動の差

- director は TS と同じく完全に逐次で回す (TS の rounds エンジンに `Promise.all` は無い)。`parallel_map` は使っていない
  (使うと LLM の呼び出し順が変わり、golden と投票の記録順が Node と食い違う)。
- **追加**: `FlowRunOptions::state` (`FlowRunState`) を渡すと進行の段階・ラウンド・ターン・発話数を別スレッドから読め、
  `request_stop()` で次の区切り (ターン・評価・サマリ・結論の前) に `FlowStopped` を投げて止まる。止まったときは結論を作らない
  (status='started'・結論なし = discussion-redo の「途中で止まった議論」と同じ状態で、発話は残る)。渡さなければ TS と同じ挙動。
- `run_flow` は生成キャストが空のとき `std::runtime_error` (TS は facilitator が undefined のまま後で落ちていた)。
- ラウンド数 / ターン数は `clamp_count` の結果を int にする (config の既定値が小数のとき TS は小数のままループ境界に使った。整数の config では同じ)。
- `onUtterance` の例外は TS と同じく run_flow の外へ出る。`onVote` の例外は `std::exception` だけを warn にする (TS は何でも)。
- `ensure_conclusion_cache_fresh` / `recompute_material_counts` の「全件」は `Number.MAX_SAFE_INTEGER` を LIMIT に渡す (同じ)。
  `recompute_material_counts` の算出値は double で受け、非有限・負は書かない (同じ)。読み戻しは整数 (小数の件数は切り捨て。TS は小数を保持)。
- `ConclusionSummary` の件数・時刻は int64 (DB は INTEGER)。
- `render_conclusion_markdown` は `updatedAt` が Date の範囲 (±8.64e15) 外なら `std::range_error` (TS の RangeError と同じ位置で失敗)。
- `safe_slug` の 60 単位の切り出しはサロゲートの途中で切れたとき `str::js_slice` の扱いになる (TS は孤立サロゲートを含む文字列。
  JSON に書けないため golden から外した)。
- GLAB トークンの比較は Node と同じく「ヘッダ値を latin1 として読んだものの UTF-8」と設定値を比べる。設定値に非 ASCII を含むと
  UTF-8 のまま送られた値とは一致しない (Node と同じ。テストで確認)。
- `request_url_parts` (Host の正規化) は WHATWG URL の全規則ではない: `127.1` や `[0:0::1]` のような別表記・percent-encoding の
  ホストは正規化せずに不一致として 403 にする (TS は URL の正規化で通すことがあった。拒否側に倒れる差)。Host が無いときは 403 (Node は 400)。
- `list_exported_discussions` の並びは `visualize::locale_compare` (ICU 近似、w1-F)。ASCII の議論 id では同じ。
- learning-cache の `DEFAULT_CACHE_PATH` は関数 `default_learning_cache_path()` (呼んだ時点の cwd で解決。TS は import 時の cwd)。
- `build_gap_cache` の games ディレクトリの列挙は `fs::list_dir` の名前順 (Node の readdirSync も libuv の scandir で名前順)。
- conclusion-publication の CORS / エラー時の Hono 既定応答 (500) は router 側の挙動に従う。

## 5. 土台に欲しいもの / 人間の判断が要ること

- 土台に欲しいもの
  - `http::Request` に「要求 URL (Hono の `c.req.url`)」を組む関数。今は `api::request_url_parts` に局所的に持った (Host の読み方を各所で揃えるため)。
  - Node と同じ「ヘッダ値を latin1 として読んだ文字列の UTF-8」を返す補助 (`bearer_token_of` に局所的に持った)。
  - テスト補助: UUID の正規化と記録した応答を返す偽の LLM (`tests/flow/w3l_support.hpp`) は他の flow の golden でも使える。
  - 作業環境: 全体ビルド中にディスクが満杯 (No space left on device) になった。各担当の Debug ビルド (-g) が 3GB 前後あるため。
    このビルドディレクトリは `-DCMAKE_CXX_FLAGS_DEBUG=-g0 -DCMAKE_EXE_LINKER_FLAGS=-s` で構成した (CMake のファイルは変えていない)。
- 人間の判断が要ること
  - 他の担当 (dialectic driver / foresight / web / discord-live) が `FlowUtteranceRecord` / `VoteEvent` / `FlowDirectorResult` を
    `flow/director.hpp` から使う前提にした。並行の担当が別に定義していれば統合時に一本化が要る。
  - `FlowRunState` の停止 (追加機能) を Web / Discord の「止める」操作に配線するか。
  - Host の別表記 (`127.1` 等) を拒否する差を受け入れてよいか (安全側の差)。
