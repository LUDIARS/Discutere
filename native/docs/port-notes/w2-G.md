# port-notes w2-G: ディスカッションペーパーの一式と dialectic エンジンの部品

job `cpp-node-20261030`。担当の TS 35 ファイル (約 5,900 行) を C++ に移した。
golden 生成: `npx tsx native/tests/golden/w2-G-flow.gen.ts` → `native/tests/fixtures/flow/w2g_golden.json`
(偽の LLM に同じ応答を与え、Node 側の戻り値・LLM への呼び出し (system / prompt / model / maxTokens)・警告文を記録。DB を使う経路は一時ディレクトリの SQLite)。

## 1. 機能の対応表 (TS → C++)

| TS | C++ |
|---|---|
| src/flow/cost-logger.ts | native/src/flow/cost_logger.{hpp,cpp} (`with_cost_log(LLMClient&, ctx)` が `CostLoggedClient` を返す) |
| src/flow/effect-predict.ts | native/src/flow/effect_predict.{hpp,cpp} (`extract_json_object`・`llm_json_limits`・`WarnFn` もここ) |
| src/flow/mechanic-extract.ts | native/src/flow/mechanic_extract.{hpp,cpp} (`extract_json_array`) |
| src/flow/games-md.ts | native/src/flow/games_md.{hpp,cpp} (front_matter::parse / stringify) |
| src/flow/paper-markdown.ts | native/src/flow/paper_markdown.{hpp,cpp} (`RoundSummary` もここ。正規表現は線形の走査) |
| src/flow/discussion-paper.ts | native/src/flow/discussion_paper.{hpp,cpp} (`ContextVoice`・`NewPaper` = Omit<DiscussionPaper,...>) |
| src/flow/paper-revisions.ts | native/src/flow/paper_revisions.{hpp,cpp} |
| src/flow/discord-flow-runs.ts | native/src/flow/discord_flow_runs.{hpp,cpp} |
| src/flow/paper-refine.ts | native/src/flow/paper_refine.{hpp,cpp} |
| src/flow/paper-voices.ts | native/src/flow/paper_voices.{hpp,cpp} |
| src/flow/voice-cache.ts | native/src/flow/voice_cache.{hpp,cpp} (`VoiceCache` は mutex で守る。`as_fn()` で関数として渡せる) |
| src/flow/user-voices/mix.ts | native/src/flow/user_voices/mix.{hpp,cpp} (`di::flow::user_voices`) |
| src/flow/improvement-score.ts | native/src/flow/improvement_score.{hpp,cpp} |
| src/flow/persona-setup.ts | native/src/flow/persona_setup.{hpp,cpp} |
| src/flow/round-summary.ts | native/src/flow/round_summary.{hpp,cpp} |
| src/flow/vote.ts | native/src/flow/vote.{hpp,cpp} (tally は JS オブジェクトと同じ挿入順の vector) |
| src/flow/spec-analyze.ts | native/src/flow/spec_analyze.{hpp,cpp} |
| src/flow/dialectic/store.ts | native/src/flow/dialectic/store.{hpp,cpp} |
| src/flow/dialectic/issues.ts | native/src/flow/dialectic/issues.{hpp,cpp} |
| src/flow/dialectic/issue-voices.ts | native/src/flow/dialectic/issue_voices.{hpp,cpp} |
| src/flow/dialectic/exchange.ts | native/src/flow/dialectic/exchange.{hpp,cpp} (`GroundExchange`) |
| src/flow/dialectic/goal.ts | native/src/flow/dialectic/goal.{hpp,cpp} |
| src/flow/dialectic/outcome.ts | native/src/flow/dialectic/outcome.{hpp,cpp} |
| src/flow/dialectic/position-stance.ts | native/src/flow/dialectic/position_stance.{hpp,cpp} |
| src/flow/dialectic/position.ts | native/src/flow/dialectic/position.{hpp,cpp} |
| src/flow/dialectic/rule-check.ts | native/src/flow/dialectic/rule_check.{hpp,cpp} |
| src/flow/dialectic/synthesis.ts | native/src/flow/dialectic/synthesis.{hpp,cpp} |
| src/flow/dialectic/tension.ts | native/src/flow/dialectic/tension.{hpp,cpp} |
| src/flow/dialectic/turn-prompt.ts | native/src/flow/dialectic/turn_prompt.{hpp,cpp} |
| src/flow/dialectic/exchange-prompt.ts | native/src/flow/dialectic/exchange_prompt.{hpp,cpp} |
| src/flow/dialectic/scheduler.ts | native/src/flow/dialectic/scheduler.{hpp,cpp} |
| src/flow/debatability.ts | native/src/flow/debatability.{hpp,cpp} |
| src/flow/flow-setup.ts | native/src/flow/flow_setup.{hpp,cpp} |
| src/flow/paper-reuse.ts | native/src/flow/paper_reuse.{hpp,cpp} |
| src/flow/proxy/position-attribution.ts | native/src/flow/proxy/position_attribution.{hpp,cpp} |

使い方の規約: LLM を呼ぶ関数は第 1 引数に `persona_engine::LLMClient&` を取り、残りを Args struct で受ける
(`run_synthesis_loop` だけは LLM が 3 つなので struct にポインタで持つ)。`warn` は `std::function<void(const std::string&)>`
(未指定は TS の既定どおり: `console.warn` 既定のものは `log::warn`、`() => {}` 既定のものは何もしない)。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| tests/flow/paper-markdown.test.ts, paper-provenance.test.ts (md ⇄ 構造化・出所・仮説節・論点節) | flow.w2g_golden (+ golden 13 本の md・固定欄・fallback) |
| tests/flow/effect-predict.test.ts, improvement.test.ts (正規化・ベクトル・射影・勝者) | flow.w2g_golden |
| tests/flow/mechanic-extract.test.ts, spec-analyze.test.ts | flow.w2g_golden (+ チャンク分割の境界は flow.w2g_db) |
| tests/flow/vote-conclude.test.ts, possession-vote.test.ts の投票部分 | flow.w2g_golden (parseVoteId・同点くじ・run_round_vote の DB 記録) |
| tests/flow/voice-cache.test.ts, user-voices.test.ts の混合部分 | flow.w2g_golden |
| tests/flow/paper-refine.test.ts | flow.w2g_golden |
| tests/flow/paper-revisions.test.ts | flow.w2g_db |
| tests/flow/paper-reuse.test.ts | flow.w2g_golden (番号の読み取り) + flow.w2g_db (保存・検索・使い回し) |
| tests/flow/foundation.test.ts のペーパー永続・persona-setup 部分 | flow.w2g_db / flow.w2g_golden |
| tests/flow/cost-instrumentation.test.ts の記録部分 | flow.w2g_db (cost_logger) |
| tests/flow/debatability.test.ts | flow.w2g_golden |
| tests/flow/dialectic-exchange.test.ts, goal.test.ts, outcome.test.ts, rule-check.test.ts, turn-prompt.test.ts, dialectic-scheduler.test.ts, dialectic-review-fixes.test.ts の部品部分 | flow.w2g_golden |
| tests/flow/proxy-debate.test.ts の定立の帰属 | flow.w2g_golden |
| (無し) 4 表の CRUD・議論一覧・削除・再議論・games md | flow.w2g_db |

tests/flow/dialectic-driver.test.ts / entrypoints.test.ts / discussion.test.ts は driver・director (担当外) を通す結合テストなので移していない
(部品の振る舞いは上の golden で Node と一致を確認済み)。
ctest 全体: 79/79 通過 (ビルドディレクトリ `/home/user/build-w2-G`)。

## 3. 安全の対応表

| 防御 / 入力経路 | C++ |
|---|---|
| LLM の応答 JSON (効果予測・メカニクス増補・仕様書解析・論点分解・両論武装・価値軸・定立・応酬・発話・止揚・批准) | `parse_json` を `llm_json_limits()` (4MiB / 20 万要素 / 深さ 64) で読む。全体が上限超過なら救済のブレース走査へ回り、救済の 1 要素も同じ上限。救済の走査は線形 (20 万要素・8MiB でテスト) |
| DB の JSON 列 (tags / mechanics / grounds / ratification / review_info / debatability) | `parse_json` (16MiB / 100 万要素 / 深さ 64)。壊れていれば空・null 扱い (TS と同じ) |
| DB の行 | `sql::QueryLimits` (既定 200 万行 / 512MiB。声の読み出しは 2,000 行 / 64MiB) |
| 議論一覧の state / scope | 許可リストの固定 SQL 断片のみを埋め込む (未知の値は SQL に入れず `std::invalid_argument`) |
| 議論の削除 | 表名は固定の許可リスト。1 トランザクション |
| data/games/<slug>.md の書き込み | **新たに** `fs::safe_join` で gamesDir の外を拒否。既存ファイルは 4MiB 上限で読み、読めない front matter は上書きせずに例外。書き込みは一時ファイル + rename |
| 機密タグ | ユーザ意見の代わりに synthetic の想定意見を配る (build_persona_paper、TS と同じ) |
| 代理議論の帰属検査 | proxy::check_attribution を論拠ごとに通し、通らなければ推測に落とす (TS と同じ) |
| 前提ルール・批准 | 判定 LLM の障害時の倒し方 (ok / concede / reject デフォルト / compromise / values / unresolved_fact) を TS と同じに。批准の過半数検証はコードが持つ |
| 正規化表現 | `std::regex` を使わず線形の走査 (見出し・メカニクス行・出所併記・論点注記・番号回答・ペーパー番号・事実判定)。2 万行の病的な md で時間を確認 |
| 並行 | `VoiceCache` は mutex。実 lookup はロックの外 |

## 4. 移していないもの・挙動の差

- driver / director / information-gate 等の呼び出し側は担当外。ここの部品はそれらが同期で呼べる形にした。
- `mechanics_json` 等の JSON 列のキー順は `to_json(MechanicSummary)` の固定順 (name, description, source, intended_*)。
  TS は値の作られ方でキー順が変わった (中身は同じ)。
- `parse_reuse_paper_no`: 先頭の 0 を除いて 16 桁以上の番号は nullopt (TS は 1e20 等の大きな数を返した)。
- `upsert_game_mechanics`: slug が gamesDir の外を指すと例外 (移植前は検査なし)。既存 `mechanics` に object でない要素があると
  TS は name を読めず undefined キーにまとめた / null で例外になったが、C++ は名前なしとして 1 つにまとめ、例外にしない。
- `list_flow_sessions`: 未知の state / scope は `std::invalid_argument` (TS は SQL に undefined を埋めて SQLite エラー)。
- `persona_setup`: JSON パース失敗時の警告文の理由部分は C++ のパーサの文言 (TS は V8 の SyntaxError の文言)。
- `pick_vote_winner`: rng が [0,1) を外れても範囲外を引かない (TS は undefined)。
- `vote` の tally の JSON キー順: JS は整数のように見える id ("1" など) を先に並べるが、C++ は挿入順 (値は同じ)。
- `run_synthesis_loop` / `schedule_next_turn` / `setup_flow_paper` は必須の引数 (LLM・graph・voiceCache) が無いと `std::invalid_argument`。
- `toLowerCase` は `str::to_lower` (ASCII・Latin-1・全角英字)。ラベル判定 (yes/no, ok, rebut 等) と検索語の一致に影響する文字種の範囲は同じ。
- 効果予測の golden で Node の `Math.sin` と libm の最後の桁が違うことがあり、射影スコアは 1e-12 の誤差で比べた (実装の差ではない)。

## 5. 土台に欲しいもの / 人間の判断が要ること

- 土台に欲しいもの
  - JS の `Array#sort` 既定 (UTF-16 コード単位順) の比較関数を `base/strings` に (`voice_cache.cpp` と `vote.cpp` に局所的に持った)。
  - JS の `\s`/`.` 相当の「行終端」判定 (`\n \r U+2028 U+2029`) を `base/strings` に (paper_markdown.cpp に局所的に持った)。
  - `split(/\r?\n/)` (bare `\r` で割らない版) を `base/strings` に (`str::split_lines` は `\r` 単独でも割る)。
  - LLM 応答の JSON 抽出 (`extract_json_object` / `extract_json_array`) は facilitator/prompts と flow で別実装がある。共通化するなら base か persona_engine に。
- 人間の判断が要ること
  - games md の slug のパス検査で例外にしてよいか (学習フローが外部由来の名前から slug を作る場合に失敗として表に出る)。
  - 議論番号の桁上限 (15 桁) でよいか。
