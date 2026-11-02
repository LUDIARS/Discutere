# port-notes w4-P: dialectic エンジンの駆動・4 フローの振り分け・改善・壁打ち・未来予測

job `cpp-node-20261030`。担当の TS 10 ファイル (約 2,460 行) を C++ に移した。
golden 生成: `npx tsx native/tests/golden/w4-P-flow.gen.ts` → `native/tests/fixtures/flow/w4p_golden.json`。

golden は w3-L と同じ方式: プロンプトの印で応答を決める偽の LLM (tests/flow/dialectic-driver.test.ts のルーターに
rounds エンジン・効果予測・未来予測の段を足したもの) で Node 側の議論を最後まで回し、LLM への呼び出し列
(system / prompt / model / maxTokens) と応答、DB に残る全表の全行 (時刻・latency の列を除く)、onUtterance / onVote、
log / warn、外部の声の検索呼び出し、戻り値 (壁打ちは各 submitUser の戻り値、未来予測は commit した発話とペーパー本文)
を記録する。C++ 側は `tests/flow/w3l_support.hpp` の `ReplayLLM` / `Normalizer` / `TempFlowDb` / `dump_db` / `lcg` を
再利用して、記録した応答を順に返し、同じ呼び出し列・同じ行・同じ通知・同じ戻り値になることを確かめる。
シナリオは 23 本:
dialectic 直接 14 本 (basic・事実対立 未解決/解決・批准 reject・折衷・論破・双方崩壊・人が指定した論点・定立の向きの作り直しと
「認める」の検証・前提ルール違反 3 種・degrade (論点分解の LLM 失敗 / 開幕の LLM エラー / 応酬の LLM エラーと空応答 /
応答ターン上限 / 議論適性の注記)・代理 E2E)、dispatch 2 本 (engine=dialectic + 未来予測 (発言の失敗 1 件で degraded) /
engine=rounds の改善 + 未来予測 + 機密 (外部の声を一度も引かない・現状ベクトルはベースラインに落ちる))、改善 2 本
(効果予測 / 効果予測全滅 → lexicon-fallback + onVote の例外)、壁打ち 2 本 (まとめて・おわり・終了後・LLM エラーと空応答・
機密 / 暴走ガード)、未来予測フェーズ 3 本 (発言失敗 + 別モデル + 文脈 / 提案失敗 / 空応答 + 予測失敗 + 長文の分割)。

## 1. 機能の対応表 (TS → C++)

| TS | C++ |
|---|---|
| src/flow/vote-display.ts | native/src/flow/vote_display.{hpp,cpp} (`VoteDisplay` = Pick<VoteEvent,...>、`should_stack_vote_reactions` / `has_visible_vote_result` / `build_vote_ranking_lines` / `vote_notice_title`) |
| src/flow/dialectic/conclusion.ts | native/src/flow/dialectic/conclusion.{hpp,cpp} (`IssueSettlement` / `settlement_to_round_summary` / `collect_unresolved_facts` / `conclude_dialectic`) |
| src/flow/dialectic/exchange-runner.ts | native/src/flow/dialectic/exchange_runner.{hpp,cpp} (`RecordArgs` / `PositionsBySide` / `GroundExchangeArgs` / `pick_side_speaker` / `run_ground_exchange` / `write_back_ground_states`) |
| src/flow/dialectic/driver.ts | native/src/flow/dialectic/driver.{hpp,cpp} (`ensure_debaters` / `run_dialectic_flow` / `run_dialectic_discussion_flow` / `run_dialectic_improvement_flow`) |
| src/flow/improvement.ts | native/src/flow/improvement.{hpp,cpp} (`ImprovementFlowDeps` / `persist_improvement_scores` / `run_improvement_flow`) |
| src/flow/sparring.ts | native/src/flow/sparring.{hpp,cpp} (`SparringDeps` / `SparringTurnResult` (+to_json) / `SparringSession`) |
| src/flow/foresight/camps.ts | native/src/flow/foresight/camps.{hpp,cpp} (`build_foresight_cast` / `normalize_camp_turns` / `camp_stage_at` / `build_camp_turn_order`) |
| src/flow/foresight/phase.ts | native/src/flow/foresight/phase.{hpp,cpp} (`split_for_post` / `label_parts` / `run_foresight_phase`) |
| src/flow/foresight/after-flow.ts | native/src/flow/foresight/after_flow.{hpp,cpp} (`foresight_requested` / `run_with_foresight` / `continue_with_foresight`) |
| src/flow/dispatch.ts | native/src/flow/dispatch.{hpp,cpp} (`parse_flow_kind` / `DispatchDeps` / `DispatchInput` / `DispatchResult` / `FlowTypeRequiredError` / `dispatch_flow`) |

### 後続の担当 (discord-live / web routes / slack) 向けの使い方

- `dispatch_flow(input, deps)` は同期 API。呼んだスレッドで議論を最後まで進める。HTTP / Gateway のハンドラからは
  `background_executor().submit` で呼ぶ (TS の `void dispatchFlow(...)`)。`deps.llm` は `persona_engine::LLMClientPtr`
  (壁打ちのセッションが LLM を持ち続けるため shared_ptr)。
- 進行の問い合わせと停止: `deps.state = std::make_shared<FlowRunState>()` を渡すと rounds (w3-L) / dialectic の両エンジンで
  `snapshot()` (phase / round / rounds / turn / utterances) が読め、`request_stop()` で次の区切りに `FlowStopped` が
  dispatch_flow の外へ出る (発話は永続済み・結論なし = discussion-redo の「途中で止まった議論」と同じ状態)。
  dialectic の rounds は論点の数、round は論点の番号 (ordinal)。区切りは 論点の開始・発話の永続の直後 (onUtterance の後)・
  投票・結論の前。未来予測の後段は止めない (本議論が止まれば未来予測の状態は failed になる)。
- 壁打ち: `DispatchResult::session` (`shared_ptr<SparringSession>`)。`submit_user` は内部の mutex で直列化するので、
  Discord の返信と Web の say が同時に来ても状態が壊れない (sparringMaxTurns の暴走ガードも同じ)。
- 投票の表示 (`vote_display`) は `VoteDisplay{kind, tally, winner}` を受ける (VoteEvent から詰め替える)。

## 2. テストの対応表 (TS テスト → ctest 名)

| TS テスト | ctest |
|---|---|
| tests/flow/dialectic-driver.test.ts (全シナリオ: 完走・4 表整合・事実対立 2 種・批准 reject・折衷・論破・双方崩壊・人が指定した論点・定立の向き・認めるの検証・前提ルール違反 3 種・dispatch の engine 分岐) | flow.w4p_flow (golden dlx-* と dispatch-dialectic-foresight + 「golden の中身」ケース) |
| tests/flow/proxy-debate.test.ts の dialectic 代理モード E2E | flow.w4p_flow (golden proxy-e2e) |
| tests/flow/improvement.test.ts の統合 (効果予測で世論・vote 不使用・method 永続) | flow.w4p_flow (golden improvement-effect) |
| tests/flow/effect-predict.test.ts の統合 (効果予測全滅 → lexicon-fallback) | flow.w4p_flow (golden improvement-fallback) |
| tests/flow/vote-lenses.test.ts の vote-display と改善の onVote (kind=scores) | flow.w4p_flow (golden voteDisplay / improvement-* の voteEvents) |
| tests/flow/sparring.test.ts のセッション動作と暴走ガード | flow.w4p_flow (golden sparring / sparring-guard + 並行の submit_user) |
| tests/flow/foresight.test.ts のキャスト・話者順・段階・分割・フェーズ・dispatch 後段 (prompt / markdown / state は w1-F の flow.foresight 済み) | flow.w4p_flow (golden casts / turnOrders / splits / foresight-* + 「dispatch 後段」「ペーパー追記の失敗」ケース) |
| tests/flow/entrypoints.test.ts の parseFlowKind / dispatchFlow の振り分けと flow 必須 | flow.w4p_flow (golden flowKinds + 「dispatch: ラベルごとに正しいドライバ」) |
| tests/discord-hook/forum-flow-tags.test.ts / tests/slack-entry.test.ts の parseFlowKind 部分 | flow.w4p_flow (golden flowKinds) |
| tests/flow/dialectic-exchange.test.ts の pickSideSpeaker / tests/flow/outcome.test.ts の settlementToRoundSummary | flow.w4p_flow (golden pickSide / settlements) |
| w2-G・w3-L が「担当外の driver を通す」として移さなかった結合 (dialectic-driver.test.ts / entrypoints.test.ts / discussion.test.ts の dispatch 部分) | flow.w4p_flow (上と同じ) |
| (無し) ensureDebaters・進行状態と停止・境界 | flow.w4p_flow |

entrypoints.test.ts の parseForumEntry / handleForumFlowPost (entry-discord) と Web ルート部分は担当外 (Web / Discord の担当)。

## 3. 安全の対応表

| 防御 / 入力経路 | C++ |
|---|---|
| 機密 / 内部タグの外部の声遮断 | dispatch で `gate_external_voices` を通した検索関数だけを本議論・壁打ち・未来予測に渡す (TS と同じ)。golden dispatch-improvement-confidential で検索が一度も呼ばれないこと、注入ドライバのテストで `listExternalVoices` が空・未来予測の prompt に声が載らないことを確認 |
| 代理議論の帰属検査 | 定立は `proxy::attribute_position_grounds`、反論は `proxy::check_attribution` を通し、通らなければ問いに格下げ + 進行役の注意 (TS と同じ)。golden proxy-e2e で確認 |
| 前提ルールのゲート / 「認める」の検証 | `gate_rebut` / `judge_concession` を TS と同じ条件で通す (data が空の反論は論拠にしない) |
| LLM の応答 (定立・応酬・止揚・批准・効果予測・未来予測) | 各部品 (w2-G / w1-F) の `parse_json` 上限 (`llm_json_limits`) で読む。2MiB の `{` だけ・壊れた JSON・常に失敗の 3 種で dialectic が完走することをテスト |
| 暴走ガード | 壁打ち `sparringMaxTurns` は TS と同じ (並行の 4 スレッド × 5 発言でも AI 応答は上限ちょうど)。応酬は `exchangeTurns`、論点は ISSUES_MAX、未来予測の発言回数は `normalize_camp_turns` に **新たに** 上限 1000 (`MAX_CAMP_TURNS`) |
| 投稿の分割 | `split_for_post` はコードポイントで数え、壊れた UTF-8・8MB の入力も線形に処理 (上限ちょうど / +1 / limit 0 をテスト) |
| ラベル解決 | `parse_flow_kind` は 1MiB のラベルでも線形 (正規表現なし) |
| 並行 | 壁打ちセッションは mutex で直列化、`is_ended` は atomic。dialectic の状態は FlowRunState (mutex + atomic) 経由でだけ外から読む |
| 外部 I/O の期限 / 同時実行 | driver・dispatch は TS と同じく逐次 (並行の LLM 呼び出しは無い)。LLM の期限は各 LLMClient の既定 |

lorica の課金を新たに入れた入力経路は無い (外部入力の受け口は持たず、LLM 応答の JSON は部品側で課金済み)。

## 4. 移していないもの・挙動の差

- 全ファイル移した (Node に残すものは無い)。
- **追加**: dialectic でも `FlowRunOptions::state` を読む (w3-L の rounds と同じ仕組み)。停止の区切りに「発話の永続 + onUtterance の直後」を
  含むので、応酬の途中でも止まる (TS には停止が無い)。
- `normalize_camp_turns`: 非有限 (NaN) は TS と同じく 2。Infinity は TS では無限ループになるところ、C++ は `MAX_CAMP_TURNS` (1000) で止める。
- `camp_stage_at` の負の index は先頭の段 (TS は undefined)。
- `split_for_post` の limit 0 は 1 として扱う (TS は無限ループ)。
- `build_vote_ranking_lines` の並びは tally の挿入順を安定に保つ (JS の Object.entries は整数に見えるキーを先に並べる。w2-G と同じ差)。
- `ensure_debaters` / 代理の参加者数の誤りは `std::runtime_error` (TS は Error)。`run_ground_exchange` / `run_foresight_phase` は必須の引数
  (exchange・commit 等) が無いと `std::invalid_argument`。
- `SparringSession::submit_user` を `start()` の前に呼ぶと `std::logic_error` (TS は未定義の paper で落ちていた)。sessionId は構築時に採番 (TS と同じ)。
- `run_improvement_flow` の tally は同じ id が重なれば後の値で上書き (JS のオブジェクトと同じ結果、位置は最初の挿入)。
- `dispatch_flow` の learning / sparring 分岐は TS と同じく残したが、`parse_flow_kind` は学習・壁打ちのラベルを持たない (TS と同じ) ので
  ラベル経由では到達しない。
- `onVote` の例外は `std::exception` だけを warn にする (w3-L と同じ)。onUtterance の例外 (FlowStopped を含む) は外へ出る。

## 5. 土台に欲しいもの / 人間の判断が要ること

- 土台に欲しいもの
  - テスト補助の `w3l_support.hpp` (ReplayLLM / Normalizer / TempFlowDb / dump_db) を `tests/flow/` 共通の名前で置き直すと良い
    (今は w4-P も w3l の名前で使っている)。golden の読み込みだけ自前に持った。
  - golden fixture が 4.5MB ある (system / prompt の全文と DB 全行を 23 シナリオ分)。fixture のサイズ方針が要るなら決めてほしい。
- 人間の判断が要ること
  - `FlowRunState` の停止を Web / Discord の「止める」操作に配線するか (後続の担当)。
  - 未来予測の発言回数の上限 (1000) と limit 0 の扱いを受け入れてよいか (TS では無限ループになる入力への安全側の差)。
