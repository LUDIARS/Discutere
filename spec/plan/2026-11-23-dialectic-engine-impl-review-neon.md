---
task: dialectic-engine-impl-review
project: Discutere
reviewer: neon
base_commit: e796c5a
created: 2026-11-23
---
# Discutere 議論エンジン (C++ 版 `native/`) 実装評価レビュー — Neon

## 総合評価: 2 / 5

**一言の理由**: 論証状態機械（Issue / Position / Tension / Synthesis）の骨格と障害時 degrade は堅実に C++ へ移植されているが、(1) 進行文レンダリング・論点ごとの中立投票・折衷後の批准による高コストモデルの大量浪費、(2) `opinion` 枠・改善スコア・憑依・計測型収束の死蔵／破綻、(3) v2 改善設計（P1〜P11）全件未実装による「打ち切り隠蔽・深さ上限バイアス・撤回論拠の結論復活」が残存しており、このまま改修を積むとコストと結論信頼性の歪みが拡大する。

---

## 観点 1: 議論の流れの正しさ

### 1-1. [重大] `opinion` ロール（および `roster.discussants` 先頭の最上位モデル）が dialectic で一度も発言しない死んだ枠になっている
- **根拠**:
  - `native/src/flow/personas.cpp:149-160`: `generate_flow_personas` は `remaining / 3` 以上（最小 1 人、既定 `personaCount=4` で 1 人、運用編成 6 人で 2 人）を `role="opinion"`, `stance="opinion"` として生成し、`roster.discussants` の先頭から順にモデルを割り当てる（`discutere.config.example.json:29-36` の編成では先頭の `opus@xhigh` と `opus@medium` が `opinion` 枠になる）。
  - `native/src/flow/dialectic/driver.cpp:176-184, 396`: 定立は `pro` / `con` の 2 人のみ。
  - `native/src/flow/dialectic/exchange_runner.cpp:33-47, 118-120`: `pick_side_speaker` は `persona_stance(p) != side`（`side` は `"pro"` または `"con"`）で候補を絞るため、`persona_stance(p) == "opinion"`（`native/src/flow/personas.cpp:120`）のメンバーは一度も選ばれない。
- **影響**:
  - `OVERVIEW.md §5.2` が定める「意見屋（ユーザ意見のまとめを参考値に持つ）」の役割が dialectic には存在せず（代わりに全 debater に `issue_voices` を直接配布、`native/src/flow/dialectic/driver.cpp:378-380`）、`opinion` 枠は `setup_session_personas` で価値軸を作るコストだけ払って 0 発言のまま終わる。
  - 運用編成では最も高価な `opus@xhigh` / `opus@medium` が議論に 1 回も参加しない結果になる。

### 1-2. [重大] 改善フロー (`flow=improvement`) が dialectic 既定昇格により機械スコア評価を失っている
- **根拠**:
  - `native/src/config.cpp:295`: `flow.engine` の既定は `"dialectic"`。
  - `native/src/flow/dispatch.cpp:137-143`: `engine == "dialectic"` のとき `run_dialectic_improvement_flow` へ分岐。
  - `native/src/flow/dialectic/driver.cpp:653-674, 759-765`, `native/src/flow/dialectic/driver.hpp:43, 52`: `run_dialectic_improvement_flow` は `flow="improvement"` を渡すだけで、`options.evaluateRound` を使わず無条件で中立 LLM 投票 (`run_round_vote`) を呼ぶ。
  - `native/src/flow/improvement.cpp:32-115`: `design_gap` + `effect_predict` による機械スコア評価は `rounds` エンジン専用に留まっている。
- **影響**: `OVERVIEW.md §11(b)` / `discussion.md §6` の「改善フローは機械計算 (`design_gap` / 効果予測) が世論を決める」仕様と既定動作が食い違っている。さらに `native/src/flow/vote.cpp:142` が cost log の `flow` を `"discussion"` 固定で書き込むため、改善フローの投票コストが `discussion` に誤計上される。

### 1-3. [重大] 計測型収束 (`dialectic.md §6`) の 3 シグナルのうち 2 つが実装上破綻している
- **根拠**:
  - **シグナル (a) `unresolvedRebuttals == 0`**: `ArgumentGraph` はセッション全体で 1 インスタンス（`native/src/flow/dialectic/driver.cpp:194`）で論点ごとにリセットされない。深さ上限到達ノード（`native/src/flow/dialectic/exchange.cpp:133` で `status="cut"`）やターン上限打ち切りノード（`native/src/flow/dialectic/exchange_runner.cpp:111-116`）は `argument_graph.cpp:67` に `state="challenged"` のまま残るため、論点 1 で 1 件でも上限打ち切りが発生すると、論点 2 以降も `graph.unresolved_rebuttals()` がセッション終了まで永遠に非ゼロとなり、(a) は二度と成立しない。
  - **シグナル (c) `newClaimsInWindow == 0`**: 止揚経路の末尾 `native/src/flow/dialectic/driver.cpp:636` で `graph.apply(..., "synthesize", ..., ++global_turn)` を呼ぶが、`native/src/flow/argument_graph.cpp:47-49` が `"synthesize"` を `ClaimNode` として登録する。直後の `native/src/flow/dialectic/driver.cpp:698` の `graph.new_claims_since(global_turn - staleTurns)` が直前の止揚自身を必ず拾うため、止揚経路を通った論点では (c) が構造上絶対に成立しない。
- **影響**: 実質的に「未応答反論 0（打ち切り無し）かつ 投票集中度 ≥ 0.6」または `aufhebungTarget` キャップでしか早期収束しない。さらに `paper-gate` 経由では `driver.cpp:704-706` によりそもそも打ち切らないため、Web 正規フロー等では収束判定自体がログ出力以上の意味を持たない。

### 1-4. [注意] 事実対立の照会が `VoiceCache` をバイパスし、件数も論点配布と食い違う
- **根拠**:
  - 論点開始時は `voice_cache.lookup({issue.title, theme}, cfg.flow.paperRichness.voices)`（既定 15 件、`native/src/flow/dialectic/driver.cpp:378`）。
  - 事実対立の照会は `voice_cache` ではなく生の `list_external_voices` を直接渡し（`native/src/flow/dialectic/driver.cpp:587`）、`native/src/flow/dialectic/tension.cpp:10, 110` が `EVIDENCE_LIMIT = 10` 件で別クエリを投げる。
- **影響**: キャッシュが効かないうえ、論拠で議論者が参照した 15 件と事実照会が見る 10 件の集合・番号が一致しない。

### 1-5. [注意] `dialectic` と `rounds` の機能ドリフト・死んだコードの残存
- **根拠**:
  - **未配線機能**: `flow.paperRefine.enabled` は `rounds`（`native/src/flow/director.cpp:452-479`）にしか実装がなく、`native/src/flow/dialectic/driver.cpp` には存在しない。
  - **旧スケジューラの死んだコード**: `native/src/flow/dialectic/turn_prompt.cpp:22-161`（`build_dialectic_turn_prompt`, `parse_turn_response`）と `native/src/flow/dialectic/scheduler.cpp:54-105`（`schedule_next_turn`, `nomination_fallback_text`）は `driver.cpp` から一切呼ばれていない（`render_position_digest` と `render_facilitator_line` / `render_nomination` / `quote_excerpt` のみ現役）。`config.cpp:296` の `rebutTurns` や `migrations.cpp:342-355` の `flow_alignment` も未使用のまま残存。
  - **代理モードの二重立場判定**: 代理議論では `driver.cpp:330-345` で `proxy::judge_proxy_stance` を 2 人分呼んで賛否を決めた直後、定立後の `driver.cpp:418-436` で通常の `judge_position_stance` も走る。安全網としては動くが、やり直し時に束外の推測を誘発しうる。

### 1-6. [軽微] `degrade` 時の論拠投稿における `utteranceId` 共有
- **根拠**: `native/src/flow/dialectic/driver.cpp:464-480`。定立の論拠 `say` が `o.gp.text` と一致すると新規投稿せず `o.claimId` を流用する。通常は 1 論拠だが、万一複数論拠が同文に degrade すると複数 `ArgumentNode` が同一 `utteranceId` を指し、`ArgumentGraph` 上の辺が衝突する（推測: 実運用で複数論拠が主張と完全一致する頻度は低い）。

---

## 観点 2: LLM 呼び出しの費用対効果

### 2-1. 1 論点・1 議論あたりの LLM 呼び出し内訳見積もり

運用設定例（`discutere.config.example.json:16-37`: `facilitator="opus"`, `judgeModel="sonnet"`, `finalJudgeModel="fable"`, `voterCount=3`, `exchangeTurns=16`, `argumentDepth=4`, 3〜4 論点、論拠 2〜3 件/陣営、応酬 10〜16 ターン想定）での内訳:

| 役割 / 処理 | 実装箇所 | 適用モデル (運用設定例) | 1 論点あたり回数 | 1 議論 (3〜4 論点) | 決着・結論への効き |
|---|---|---|---:|---:|---|
| ペルソナ価値軸生成 | `driver.cpp:166`, `persona_setup.cpp:96` | `personaSetupModel` (空=既定) | — | 1 | 中（ただし `opinion` 枠分は無駄） |
| [0] 論点分解 | `driver.cpp:274` | `facilitator.model` (`opus`) | — | 0〜1 | 高（paper-gate 時は 0） |
| 進行文: 開幕 | `driver.cpp:370`, `scheduler.cpp:107` | `facilitator.model` (`opus`) | 1 | 3〜4 | **極低**（定型文の口語化のみ） |
| [1] 定立生成 | `driver.cpp:416, 434` | `pro`/`con` 各モデル | 2 (再生成で最大 4) | 6〜12 | 高 |
| [1] 定立向き判定 | `driver.cpp:429` | `judgeModel` (`sonnet`) | 2 | 6〜8 | 中（`haiku` 級で十分） |
| [2] 進行文: 再反論指名 | `exchange_runner.cpp:137`, `scheduler.cpp:151` | `facilitator.model` (`opus`) | **6〜12** (深さ≥1 ごと) | **18〜48** | **極低**（定型枠の口語化に `opus`） |
| [2] 論拠応酬ターン | `exchange_runner.cpp:155` | `pro`/`con` 各モデル | 4〜16 | 12〜64 | 高 |
| [2] 前提ルール / 認める判定 | `exchange_runner.cpp:176, 232` | `judgeModel` (`sonnet`) | 4〜16 | 12〜64 | 中〜高（高頻度、`haiku` 分離余地大） |
| [2] 進行文: まとめ | `exchange_runner.cpp:274` | `facilitator.model` (`opus`) | 1 | 3〜4 | **極低〜負**（数値集計の改変リスク） |
| [3] 対立型分類 | `driver.cpp:566` | `judgeModel` (`sonnet`) | 0〜1 | 0〜4 | 中（4 択、`haiku` 級で十分） |
| [3] 事実照会判定 | `driver.cpp:590` | `judgeModel` (`sonnet`) | 0〜1 | 0〜2 | 中（3 択） |
| [4] 止揚候補生成 | `synthesis.cpp:229` | `facilitator.model` (`opus`) | 0〜3 (修正 ≤2) | 0〜12 | 高 |
| [4] 折衷ゲート | `synthesis.cpp:230` | `finalJudgeModel` (**`fable`**) | 0〜3 | 0〜12 | **低**（1 文判定に最上位モデル＋後続を止めない） |
| [4] 敵対的批准 | `synthesis.cpp:242` | `finalJudgeModel` (**`fable`**) | 0〜6 (2 陣営×最大 3 周) | 0〜24 | 高（ただし折衷案への 2 回は完全な無駄） |
| 論点ごとの中立投票 | `driver.cpp:670`, `vote.cpp:145` | **未指定 (既定モデル)** | **3** (`voterCount`) | **9〜12** | **極低〜負**（決着に効かず結論を汚染） |
| 最終結論生成 | `conclusion.cpp:79, 87` | **未指定 (既定モデル)** | — | 1〜2 | 高（モデル未指定が逆に問題） |
| **合計** | | | **約 25〜62 回** | **約 75〜240 回** | `dialectic.md §9` 想定 (30〜50 回) を大幅超過 |

### 2-2. [重大] 進行文レンダリング (`render_facilitator_line`) が `opus` を 1 議論 24〜56 回浪費している
- **根拠**:
  - `native/src/flow/dialectic/driver.cpp:370-374`（開幕）、`native/src/flow/dialectic/exchange_runner.cpp:125-139`（深さ≥1 の再反論ごと）、`native/src/flow/dialectic/exchange_runner.cpp:274-279`（まとめ）がすべて `facilitator.model`（運用設定では `opus`）で `render_facilitator_line`（`native/src/flow/dialectic/scheduler.cpp:107-121`）を呼んでいる。
  - `discutere.config.example.json:14` のコメント自体が「進行文レンダリングは小モデル (`judgeModel`)」と説明しているのに、実装は `facilitator.model` になっており仕様・コメントと逆行している。
- **所見**: 引用本文は既に `scheduler.cpp:154` がコードで `quote_excerpt` を付けている。定型テンプレート（`nomination_frame_text` や集計文）をそのまま出せば **LLM 0 回** で済み、特にまとめ文は LLM に言い換えさせない方が P1 の「打ち切り・集計内訳」を正確に保てる。どうしても口語化したい場合でも `haiku` 級かローカルモデルに落とすべき。

### 2-3. [重大] dialectic の論点ごとの中立投票 (`run_round_vote`) はコスト・品質の両面で削るべき筆頭
- **根拠**:
  - `native/src/flow/dialectic/driver.cpp:653-687`, `native/src/flow/vote.cpp:123-175`: 論点決着の**後**に、その論点の非ファシリテーター発話すべて（主張 + 論拠 2〜6 件 + 応酬最大 16 件 = 最大 24 発話。撤回 `concede` やルール違反格下げ `question` も除外しない、`driver.cpp:658`）を 3 回 (`voterCount=3`) 毎回フル投入している。
  - しかも `native/src/flow/vote.cpp:143-145` は `call.model` も `call.maxTokens` も指定していないため、運用時の既定モデルで実行される。
  - 結果の用途は (1) 収束シグナル (b)（`paper-gate` では無効、最終論点でも無効、他シグナルも破綻）、(2) 結論プロンプトの `top_opinions`（撤回された発話でも 1 票取れば「高評価意見」として結論に混入する有害経路）、(3) Discord リアクション可視化のみ。
- **削除時の必須連動修正**: `native/src/flow/conclusion.cpp:75` が `if (!args.allAufhebung.empty() || !top_utterances.empty())` となっているため、**投票を単純削除すると「止揚 0 件（論破や結論なしのみ）」の議論で結論 LLM が走らず無条件 `(結論なし)` になる**。投票を削る際は (a) `conclusion.cpp:75` の起動条件を「論点決着あり (`!args.rounds.empty()`)」に改め、(b) `transcript/snapshot.cpp:57-91` や `visualize/flow_conclusions.cpp:159-165` のハイライトを「残存根拠・止揚」ベースへ移す必要がある。

### 2-4. [重大] 折衷ゲート (`elevation_gate`) の配置・モデル選定が重複と無駄撃ちを生んでいる
- **根拠**:
  - `native/src/flow/dialectic/synthesis.cpp:230-243, 266`: `elevation_gate` が `"compromise"` を返しても、直後に両陣営の `ratify_synthesis` を **`finalJudgeModel` (`fable`) で 2 回** 実行し、両者 accept なら `"compromised"`（= 結論なし）で終了する。
  - `native/src/flow/dialectic/synthesis.cpp:106-134`: `elevation_gate` 自体は `elevates` の 1 文だけを見て `aufheben` / `compromise` を返す 2 値分類だが、`driver.cpp:622` により最上位モデル `fable` で呼ばれる。しかも統合案本文も両陣営の主張も見ていないため、実質折衷なのに `elevates` だけ美辞麗句の案を見抜けない。
- **改善案**: `elevation_gate` で `compromise` と判定した時点で批准を飛ばし（`fable` 2 回削減）修正フィードバックに回すか、あるいは `elevation_gate` を廃止して `ratify_synthesis` のチェックリスト項目に「新しい枠の導入か（単なる折衷ではないか）」を統合すれば、1 候補あたり `fable` 1 回（最大 3 回/論点）を丸ごと削りつつ文脈付きで判定できる。

### 2-5. [注意] 高頻度の単一ラベル判定への `sonnet` 適用と、`maxTokens` / `model` の指定漏れ
- **根拠**:
  - `discutere.config.example.json:21` で `judgeModel="sonnet"` を全通常判定に共用しているため、単一ラベルの `judge_position_stance`（`position_stance.cpp:37-48`）、`judge_concession`（`rule_check.cpp:145-165`）、`classify_tension`（`tension.cpp:60-77`）も `sonnet` で走る。これらは `haiku` 級で十分。
  - `native/src/flow/conclusion.cpp:77-79`（最終結論生成）は `call.model` 未指定で、議論の最終出力なのに `facilitator.model` が渡っていない。
  - `native/src/persona_engine/llm/claude_cli.cpp:33-42`, `codex_cli.cpp:54-60` は `LLMInvokeArgs.maxTokens` を CLI 引数へ渡していないため、判定系の `maxTokens = 20 / 50 / 100` は CLI 経路では強制されない（出力長はプロンプト指示依存）。

### 2-6. 評価仕組みの仕分けまとめ（削る / 安くする / 残す）
- **削ってよい**: (1) dialectic の `run_round_vote`（`conclusion.cpp:75` 改修とセットで廃止）、(2) 進行文 `render_facilitator_line` / `render_nomination` の LLM 呼び出し（テンプレート直出し）、(3) 折衷確定後の `ratify_synthesis`（または `elevation_gate` の `ratify` 統合）。
- **安いモデル (`haiku` 級) に落として残す**: `judge_position_stance`, `judge_concession`, `classify_tension`, `resolve_fact_tension`（必要に応じ `ruleCheck` のみ `sonnet` 分離も可）。
- **残すべき**: `check_rebut_anchor`（コード検査・0 コスト）、`gate_rebut` の `self_contradiction` / 前提逸脱チェック、`ratify_synthesis` の敵対的批准＋過半数機械検証、`proxy::check_attribution`（コード検査・0 コスト）。

---

## 観点 3: ペルソナの文脈

### 3-1. [重大] dialectic の発話プロンプトに「遊び手の型」も「ゲームの型」も一切載っていない
各プロンプトに実際に載っている要素（C++ 正本から確認）:

| 要素 | 定立 (`position.cpp:22-48`) | 応酬 (`exchange_prompt.cpp:32-102`) | `system` (`flow_setup.cpp:88-91`) |
|---|---|---|---|
| 名前・話し方・traits | 載る（ただし `personas.cpp:45-52` の固定 3 語が全 debater 同一） | 載る（同左） | — |
| 価値軸 (`valueAxis`) | 載る（テーマ名のみから生成、`persona_setup.cpp:63-73`） | 載る（同左） | — |
| 核主張 (`coreClaims`) | 載る | 載らない | — |
| プール人物像 (`possession.descriptor` / `preferenceAxes` / `aversions` / `mechanicReactions`) | **載らない** | **載らない** | — |
| 代理の人物束 (`proxy.bundleText`) | 代理時のみ載る (`position.cpp:33`) | 代理時のみ載る (`exchange_prompt.cpp:61`) | — |
| ゲームの資料（メカニクス表・AI 試遊レビュー等） | — | — | 載る（`discussion_paper.body_md`） |
| **ゲームの型（アーキタイプ・コアループ・期待体験）** | **載らない** | **載らない** | **載らない** |
| 外部の声 (`voicesBlock`) | 載る（15 件、`driver.cpp:378-380`） | 載る（同左） | 参照方針のみ |

- **根拠（憑依の完全断絶）**:
  - `native/src/flow/director.cpp:149-164, 274` にある `apply_theme_possession` は **`native/src/flow/dialectic/driver.cpp` では一度も呼ばれていない**。
  - `native/src/flow/persona_pool.cpp:313-326`（`to_flow_persona`）は、プールから `personaIds` を明示指定しても `preferenceAxes` / `aversions` / `attributes` / `mechanicReactions` を `FlowPersona` へコピーせず、`possession` も設定しない。
  - `native/src/flow/dialectic/position.cpp` と `exchange_prompt.cpp` のどちらにも `persona.possession` を参照するコードがない（参照するのは rounds 用の `native/src/flow/discussion_paper.cpp:219-221` のみ）。`native/src/flow/dialectic/driver.cpp:229` に `u.possessionName` へのコピーだけが残っているが、プロンプトには一度も届かない。
- **結果**: 通常 dialectic では、どの debater も全員同じ性格テンプレ（「論点を掴む / 具体例豊富 / 対立を恐れない」「芯のある主張口調」）で、テーマ名だけから作られた 1 文の `valueAxis` を持つ均質な AI になっており、「どんな遊び手が、どんな型のゲームを語っているか」が完全に欠落している。

### 3-2. [注意] `persona_setup` がペーパーも人物像も見ずに価値軸を生成している
- **根拠**: `native/src/flow/persona_setup.cpp:63-73`。`build_persona_setup_prompt` は `theme` と各ペルソナの `id / name / role / stance` しか渡さない。
- **影響**: 今後プール人物像やアーキタイプを導入した際、`persona_setup` が人物像（嗜好軸・苦手）やゲームの型を知らずに `valueAxis` / `coreClaims` を作ると、「探索型で競争が苦手な人物に『PvP ランキング重視』の価値軸を振る」といった人格矛盾が起きる。

### 3-3. [注意] アーキタイプと Histrio 人生脚本を足す筋の良い設計配置
- **ゲームの型（ゲームアーキタイプ）→ `system` 側（`discussion_paper.body_md` の `# ゲームの型` 節 → `paper_system`）**
  - `driver.cpp:137` が `paper_system` を全ターン固定の `system` としてプロンプトキャッシュに乗せているため、全話者共通の静的情報である「ゲームの型（ジャンル・コアループ・必須要素・期待体験）」は `paper_system` 側へ置くのが最も自然でトークン効率も高い。ペーパー編集ゲートで人間がジャンルを修正できる経路（`paper_markdown.cpp`）にも綺麗に乗る。
- **遊び手の型（`persona_descriptor` ＋ 将来の Histrio 人生脚本パターン）→ `user` 側（`position.cpp:30-33` と `exchange_prompt.cpp:56-61` のペルソナ定義ブロック直下）**
  - 話者ごとに変わる動的情報なので `system` に入れるとプロンプトキャッシュを破壊する。定立・応酬の双方で呼ぶ共通ヘルパ（例 `render_persona_context_block(persona)`）を用意し、`persona.proxy` が無い通常議論でのみ注入するのが安全（代理議論は `proxy-debate.md §0, §6` の通り本人発言に拘束するため絶対に混ぜない）。
  - 全 debater にプール人物像を割り当てる場合、`personas.cpp:148-160` で死蔵されている `opinion` 枠を dialectic では生成せず全議論者を `debater`（pro/con）にしたうえで、`select_possession_by_theme` 上位から嗜好軸が被らないように多様化して割り当て、かつ `persona_setup` にも descriptor を渡して `valueAxis` と整合させるのが筋。
- **Histrio 人生脚本 (`E:/Document/Ars/Histrio/spec/feature/virtual-persona-life-script.md`) 接続時の衝突リスク (注意)**:
  - Histrio 仕様 §1・§3・§5 は「脚本パターン (`until` / `after` / `never` / `always` / `almost` / `open_ended`) が反応表を通じて **立場ベクトル (`support` 等) の主因になる**」と定義している。
  - 一方 Discutere の dialectic では **賛成/反対の立場がセッション開始時に固定**され、逆向きの発言は `self_contradiction`（`rule_check.cpp:75`）で格下げされる。たとえば `never`（現状維持・反対寄り）を `pro` 側にそのまま割り当てると立場固定と衝突して反論が格下げされ続けるリスクがある。
  - 対策: Di 側では Histrio の脚本パターンを「賛否の反転」ではなく「健全側の論じ方・着眼点（例: `until`＝成立条件の厳密化、`after`＝導入後リスクの点検）」として表現層に限定するか、あるいは `support` 傾向と `pro`/`con` 割当を整合させるルールを明文化しておくべき。

---

## 観点 4: 結論の信頼性

v2 改善設計書 (`spec/plan/2026-10-13-dialectic-v2-improvement.md`) の **P1〜P3 の指摘は、現在の C++ 実装 (`e796c5a`) でも全件そのまま当てはまる**。撤回・崩れた論拠や未決論点が結論へ混入する経路は以下の 4 つ。

### 4-1. [重大] 打ち切りが「落ち着いた・崩れずに残った」として記録・露出される (P1)
- **根拠**:
  - `native/src/flow/dialectic/exchange_runner.cpp:111-116, 272-282`: `answered >= args.turnCap`（既定 16）で `exchange.cut_remaining()` を呼んで未応答ノードを打ち切った後も、まとめ文は常に「論拠のやり取りが落ち着きました。崩れずに残った論拠は…」を流す。
  - `native/src/flow/dialectic/exchange.cpp:110-118`: `standing_count` は `verdict(node.id) != "refuted"` を一括で `standing` に数えるため、打ち切り未決 (`contested`) がすべて「崩れずに残った論拠」に水増しされる。

### 4-2. [重大] 深さ上限 (`argumentDepth=4`) の構造バイアスで未決論拠が機械的に `defended` になる (P2)
- **根拠**:
  - `native/src/config.cpp:299-300`: 既定 `argumentDepth = 4`（偶数）。
  - `native/src/flow/dialectic/exchange.cpp:72-97, 133`: 深さ 0 (持ち主) → 1 (反論) → 2 (持ち主) → 3 (反論) → 4 (持ち主・`depth >= maxDepth` で `status="cut"` → `verdict="contested"`)。親へ `contested` が伝播し、根 (深さ 0) も `verdict="contested"` になる。
  - `native/src/flow/dialectic/exchange.cpp:91`: `ground_states()` が `verdict != "refuted"` を一律 **`"defended"`** に写すため、深さ上限に達した未決論拠が「守られた」扱いになる。
  - `native/src/flow/dialectic/goal.cpp:7-12, 20-38`: `detect_refutation` は `unchallenged` と `defended` を同等に「残存」と判定するため、未決のまま論破不成立となり止揚へ進む。

### 4-3. [重大] 止揚生成・敵対的批准の両方が、撤回・崩れた論拠の採用を許容・強制している (P3)
- **根拠**:
  - **止揚生成 (`native/src/flow/dialectic/synthesis.cpp:27-33, 67-77, 80-83`)**: `render_position_for_synthesis` は `conceded` / `challenged` の根拠も他の根拠と同列に並べ、`valid` 根拠 ID 集合にも全根拠を含める。「崩れた根拠を前提・`preserves` に使わない」制約も機械検証も無い。
  - **敵対的批准 (`native/src/flow/dialectic/synthesis.cpp:136-156, 159-207`)**: `build_ratify_prompt` は `position.grounds` の全根拠を**状態 (`conceded`/`challenged`) すら伏せて**列挙し、`ratify_synthesis` (`synthesis.cpp:200`) は `preserved.size() > position.grounds.size() / 2` で全根拠の過半数保存を要求する。すなわち **批准ロジック自体が「撤回・崩れた根拠を止揚案で復活させないと accept にならない」構造的インセンティブを生んでいる**。また「論点の問い・判定基準をすり替えていないか」のチェック項目も存在しない。

### 4-4. [重大] 最終結論生成が旧チャット用プロンプトと生発話 20 件＋投票勝者に依存し、決着表と乖離する (P3)
- **根拠**:
  - `native/src/flow/dialectic/conclusion.cpp:34-103`: `conclude_dialectic` が作る `RoundSummary` には論点の主張と決着 1 行しかなく、残存根拠・崩れた根拠・未決根拠の区別が渡らない。
  - `native/src/flow/conclusion.cpp:46-107` → `native/src/persona_engine/facilitator/prompts.cpp:77-96`:
    - `allUtterances` の末尾 20 件を **`!u.isError` すらフィルタせず生ログで投入**（`conclusion.cpp:57-60`）。撤回された主張・ルール違反で `question` 格下げされた反論・エラー発話・進行役の違反注意がそのまま入る。
    - 投票勝者 `top_opinions` を「評価の高かった意見 (リアクション順、重視する。「/ 人間」付きは人間の意見なので最優先で尊重する)」（`prompts.cpp:91`）という旧チャット時代の文言のまま強調投入する。
    - 結論 LLM の出力をそのまま `summary` に採用し、決着表の埋め込みも矛盾検査も無い。
- **影響**: 論点 1 が「結論なし」であっても、末尾 20 発話や投票勝者の文面から LLM が未決命題を事実前提として結論に織り込み、逆に「結論なし」論点そのものを要約から落とす経路が開いたままになっている。

---

## 観点 5: v2 改善設計 (`spec/plan/2026-10-13-dialectic-v2-improvement.md`, P1〜P11) の妥当性

### 5-1. P1〜P11 の実装状況・必要性・設計の穴

**確認結果**: base commit `e796c5a` の C++ 実装において、**P1〜P11 はすべて未実装**。

| # | 項目 | 実装済? | 今も必要? | 設計の穴・C++ 実装上の注意点 |
|---|---|---|---|---|
| **P1** | 打ち切り明示・内訳集計 | 未実装 (`exchange_runner.cpp:272-282`) | **必須** | まとめ文を `render_facilitator_line` (LLM) に通すと「打ち切り M 件」や内訳数値を LLM が丸めるリスクがある。テンプレート直出しに改めるべき。また C++ 正本では `transcript/digest.cpp` / `visualize/conclusion_markdown.cpp` 側も対象になる（設計書は旧 TS パス表記）。 |
| **P2** | `unsettled` 状態追加・`argumentDepth=5` | 未実装 (`store.hpp:14`, `exchange.cpp:91`, `config.cpp:300`) | **必須** (一部修正要) | **穴①**: `exchange.cpp:133` は `depth >= maxDepth` で `cut` → `contested` にするため、偶数 (4) でも奇数 (5) でも深さ上限に達すれば結局根は `contested` → `unsettled` になる。「5 にすれば持ち主有利が消える」は誤認であり、本質は **`contested` → `unsettled` への写像変更**。逆に `argumentDepth` を 4→5 に増やすと 1 論拠の鎖が長くなり `exchangeTurns=16` でターン打ち切りが多発する副作用がある（むしろ `argumentDepth=3` + `unsettled` の方が完走率とコストが良い）。<br>**穴②**: `unsettled` 根拠を止揚の `preserves`・批准分母にどう扱うかが未定義。 |
| **P3** | 崩れた論拠・未決論点の遮断＋結論検査 | 未実装 (`synthesis.cpp:27-207`, `conclusion.cpp:46-107`) | **最優先・必須** (重大な穴あり) | **重大な穴**: `synthesis.cpp:200` の批准過半数チェックは分母が全根拠 (`position.grounds.size()`)。P3 どおり「崩れた根拠を `preserves` に入れたら不合格」にすると、**3 根拠中 2 件崩れた陣営は残存 1 件しかなく `1 > 3/2` を物理的に満たせないため止揚が永久に不成立**になる。批准の分母とチェックリスト対象を「残存根拠（`unchallenged`/`defended`。`unsettled` の扱いも明記）」へ同時に変更する必要がある。<br>**設計改善**: 結論検査 LLM を足す前に、`flow/conclusion.cpp:57-64` から「末尾 20 生発話」「投票勝者」「旧チャット用プロンプト」を外し、構造化決着表（＋残存根拠）だけを入力にする方が根本解決になる。 |
| **P4** | 声の固定番号と md 末尾の一覧 | 未実装 (`issue_voices.cpp:19`, `conclusion_markdown.cpp:59-127`) | **必要** | **穴①**: `discussion_paper_voice` (`paper_voices.cpp:13-31`) は現在ペーパーレビュー経路でしか保存されず、`stored_voice_lookup` (`paper_voices.cpp:47-71`) も `ordinal` を返さない。`ContextVoice` に固定番号を持たせるか `VoiceCache` 側でセッション固定採番を保持する設計が要る。<br>**穴②**: 事実照会 (`driver.cpp:587`) も同じ固定番号空間に統一しないと番号ずれが残る。 |
| **P5** | `definition` 型対立追加・事実照会文言改善 | 未実装 (`tension.cpp:38-58`, `store.cpp:192`) | **必要** | **穴①**: `native/src/flow/dialectic/store.cpp:192` の `list_tensions` にホワイトリスト `one_of(type, {"facts","values","means","tradeoff"})` があり、ここへ `"definition"` を追加しないと DB 読戻し時に `"values"` に無言変換される。<br>**穴②**: `tension.cpp:38-44` の分類プロンプトには `claim` と `values` しか渡っておらず `qualifier`（限定）も根拠も無い。言葉の範囲のずれを検出するには `qualifier` と残存根拠の要旨を渡す必要がある。 |
| **P6** | 再反論指名で元の反論者を優先 | 未実装 (`exchange_runner.cpp:33-47, 119`) | **必要** (高コスパ) | 設計書の「応答対象の論拠の持ち主（深さ ≥1 なら元の反論者）」はコード上で取り違えやすい。応答対象 `node` 自体は相手陣営の発言なので、優先すべきは **`node.parentId`（1 つ上の親ノード＝自分が応答義務を負う元の論拠／反論）の `personaId`** である点を明記すべき。 |
| **P7** | 止揚の批准結果を露出に出す | 未実装 (`driver.cpp:628-631`, `dialectic/conclusion.cpp:19-23`) | **必要** (高コスパ) | データ (`SynthesisRecord.ratification`) は既に DB に保存済み（`store.cpp:207-223`）なので表示配線のみで済む。設計に大きな穴なし。 |
| **P8** | メカニクス矛盾検出 (`mechanic_mismatch`) | 未実装 (`rule_check.cpp:61-78`) | **条件付き必要** (中〜低) | **穴①**: 毎反論で呼ばれる `rule_check` に最大 30 件のメカニクス表を毎回 user 側へ載せるとトークンが激増する（現在 `rule_check.cpp:96-100` は `call.system` 空でキャッシュが効かない）。`curated` のみ短く渡すか、`paper_system` を `system` に載せてキャッシュを効かせる工夫が必須。<br>**穴②**: 仮説メカニクス (`source=="llm"`) を矛盾判定に使うと誤判定で正当な反論を弾くため `grounded_mechanics` 限定にすべき。 |
| **P9** | データなし演繹の区別・声の偏り明示 | 未実装 (`exchange_runner.cpp`, `position.cpp`) | **条件付き必要** (中) | **穴①**: 企画段階の新作（外部の声 0 件、`VOICE_REFERENCE_POLICY`）では全論拠が V 参照なしになるため、声 0 件時は警告を出さない分岐が必要。<br>**穴②**: 声の内訳（継続／離脱・不満／不明）の分類手段が未定義。LLM 追加呼び出しは避け、`debatability.cpp:70-102` 既存の決定論 `compute_evidence_balance`（20D valence 集計）を流用するのが現実的。 |
| **P10** | 版の識別 (`flow.dialectic.version = "v2"`) | 未実装 (`config.cpp`, `migrations.cpp`) | **低優先** (最後に付与) | `flow_conclusion` テーブル (`migrations.cpp:83-92`) に `engine_version` 列が無いのでマイグレーションが必要。比較検証を始める直前に足せば十分。 |
| **P11** | `claude cli exit 1` stdout 記録 & 引用末尾優先 | 未実装 (`claude_cli.cpp:139`, `scheduler.cpp:123-137`) | **必要** (即効性高) | `claude_cli.cpp:138-139, 172` に加え、`codex_cli.cpp:107-108` も exit≠0 時に stderr/stdout を一切残していないので同時修正が望ましい。`quote_excerpt`（既定 `QUOTE_MAX = 160`、`scheduler.hpp:54`）は単純末尾切りだと冒頭の主語・前提が落ちるため、上限を少し広げるか「先頭＋末尾」あるいは末尾文優先の境界テストが必要。 |

### 5-2. v2 設計書のスコープ外で見落とされている重要事項
- v2 設計書は 2026-10-13 の TS 版を前提にしており、**進行文 `opus` 浪費・論点投票の廃止・`opinion` 枠の死蔵・改善フローの dialectic 非対応・アーキタイプ文脈の導入** が含まれていない。これらを別々に触ると `driver.cpp` と `conclusion.cpp` で競合するため、統合した順序で進める必要がある。

### 5-3. v2 項目内の優先順位提案
1. **第 1 群（結論の正しさ・土台）**: **P2**（`unsettled` 導入・深さ既定見直し）→ **P1**（打ち切り明示・テンプレート化）→ **P3**（崩れた論拠遮断・批准分母修正・結論入力刷新）
2. **第 2 群（検証可能性・議論品質）**: **P4**（声の固定番号と一覧）→ **P5**（`definition` 型＋`qualifier` 入力）→ **P6**（元反論者優先）→ **P7**（批准結果露出）→ **P11**（CLI エラー可視化・引用改善）
3. **第 3 群（仕上げ・付随）**: **P9**（既存 valence 集計流用）→ **P8**（キャッシュ設計込み）→ **P10**（版識別）

---

## 観点 6: コード品質

### 6-1. [注意] 責務分割はモジュール単位で良好だが、`driver.cpp` の肥大化とエンジン間重複がある
- **良い点**: `native/src/flow/dialectic/` は状態 (`exchange.cpp`)、応酬進行 (`exchange_runner.cpp`)、ルール検査 (`rule_check.cpp`)、止揚 (`synthesis.cpp`)、永続 (`store.cpp`) に綺麗に責務分離されており、純粋ロジックは単体テストしやすい構造。
- **問題点**:
  - `native/src/flow/dialectic/driver.cpp:101-749` の `run_dialectic_flow` が 1 関数で約 650 行に肥大化しており、キャスト準備・代理分岐・定立・論拠投稿・応酬・決着・投票・収束・結論が直列に並んでいる。
  - `rounds` の `director.cpp` と `dialectic/driver.cpp` で重複配線（`check_stop`, `make_record`, `update_paper_live`, `onVote`）が多く、`apply_theme_possession` や `paperRefine` が片方にしか実装されない機能ドリフトを招いている。
  - `flow/conclusion.cpp:64` が `persona_engine/facilitator/prompts.cpp:77-96`（旧チャット用プロンプト）に依存し続けており、責務境界が濁っている。

### 6-2. [注意] ラベルパーサの単語境界欠落と DB クリーンアップ・コスト計測の抜け
- **単語境界なしの `find` による誤判定リスク**:
  - `native/src/flow/dialectic/rule_check.cpp:80-92`（`parse_rule_verdict`）、`rule_check.cpp:156-164`（`judge_concession`）、`position_stance.cpp:16-29`（`parse_stance_answer`）、`tension.cpp:46-58`（`parse_tension_type`）、`synthesis.cpp:127-131`（`elevation_gate`）はすべて素の `lower.find(label)` を使用している。
  - `"ok"` は `"look"` / `"token"` / `"not ok"` に、`"no"` は `"not"` / `"none"` / `"know"` に部分一致してしまう（同じコードベース内の `native/src/flow/proxy/stance.cpp:15-23` と `tension.cpp:23-31` では単語境界を正しく検査しているのに統一されていない）。
- **セッション削除の孤立行**: `native/src/flow/discussion_paper.cpp:460-474`（`delete_flow_session`）の削除対象テーブル一覧に `flow_issue`, `flow_position`, `flow_tension`, `flow_synthesis`, `flow_session_persona`, `discussion_paper_voice`, `flow_alignment` が抜けており、セッション削除時に dialectic 関連行が残留する。
- **コスト推定の抜け**: `native/src/persona_engine/llm/pricing.cpp:19-28`（`model_family`）が `"opus"` / `"sonnet"` / `"haiku"` しか認識せず、運用設定の `finalJudgeModel="fable"` が `nullopt` になるため、`usage.cost_usd` 非返却経路では `fable` のコスト推定が欠落する。

### 6-3. [良好 / 注意] エラー時の degrade 設計
- **良い点**: ほぼすべての LLM 呼び出しで失敗・JSON 破損・空応答時の degrade と `warn` ログが徹底されており、`ratify_synthesis` (`synthesis.cpp:170-178`) と `elevation_gate` (`synthesis.cpp:123-133`) は安全側 (`reject` / `compromise`) に倒れる。`w4p_flow_part4_tests.cpp:47-69` で異常系完走も確認されている。
- **注意点**:
  - `rule_check.cpp:101-109, 152-163` と `position_stance.cpp:43-46` は LLM 障害時に素通し（`ok` / `concede` / `unclear`）する。議論を止めない判断は妥当だが、連続障害で検査が無効化された件数が結論や要約に記録されない。
  - `native/src/persona_engine/llm/codex_cli.cpp:107-108` は exit≠0 時に stderr も stdout も記録せず原因追跡が不能。`claude_cli.cpp:138-139` も stdout を捨てる（P11）。

### 6-4. [重大] テストの有無と抜け — ゴールデン再生依存で仕様欠陥を検知できない
- **現状のテスト資産**:
  - 純粋ロジック: `native/tests/flow/small_pure_tests.cpp:47`, `w2g_golden_part3_tests.cpp`, `w2g_golden_part4_tests.cpp`（主に TS 移植等価性のゴールデン検証）。
  - 結合テスト: `native/tests/flow/w4p_flow_tests.cpp:8-68`（`w4p_golden.json` の再生）、`w4p_flow_part3_tests.cpp:137-172`（`FlowStopped` 途中停止）、`w4p_flow_part4_tests.cpp:47-78`（壊れた LLM 応答での degrade 完走・引数検証）。
- **明確なテストの抜け**:
  1. `exchangeTurns` 上限打ち切り時のまとめ文・内訳集計（`GroundExchange` の `cut_remaining` 後の結合挙動）
  2. `argumentDepth` 上限到達時の `ground_states()` と `detect_refutation()` の組み合わせ
  3. 複数論点またぎの `ArgumentGraph` と `assess_convergence`（論点 1 の未応答反論が残った状態の論点 2、止揚直後の `newClaimsInWindow`）
  4. 崩れた根拠 (`conceded`/`challenged`) がある状態での `run_synthesis_loop`（`preserves` 制約と過半数判定）および `conclude_dialectic` の結論整合性
  5. `opinion` 枠・`roster.discussants` の実発話到達性、および `flow=improvement` + `engine=dialectic` での評価経路
  6. `parse_rule_verdict` / `parse_stance_answer` / `judge_concession` の単語境界エッジケース
  7. `delete_flow_session` 後の dialectic 4 表クリーンアップ
- **改修時の注意**: 既存テストの大半が旧 TS 版挙動を焼き込んだゴールデン（`w2g_golden.json`, `w4p_golden.json`）に依存しているため、v2 改修・投票廃止・進行文テンプレート化では C++ ネイティブの意図ベース単体／結合テストを新設し、旧ゴールデンから段階的に移行する必要がある。

---

## 改修の優先順位 (上位 5 件)

1. **【最優先】LLM コスト構造の抜本整理（進行文のテンプレート化＋dialectic 投票の廃止＋折衷ゲート／判定モデル適正化）**
   - **対象**: `native/src/flow/dialectic/driver.cpp`, `exchange_runner.cpp`, `scheduler.cpp`, `synthesis.cpp`, `native/src/flow/conclusion.cpp`, `vote.cpp`
   - **理由**: 1 議論あたり最大 24〜56 回の `opus` 進行文言い換え、9〜12 回の無意味・有害な投票、折衷確定後の `fable` 2 回批准という明確な浪費を先に止めないと、後続の v2 改修・アーキタイプ導入で 1 議論のコストが破綻する。**必須の連動**: 投票廃止と同時に `conclusion.cpp:75` の起動条件を「論点決着あり」へ修正し、改善フロー (`flow=improvement`) の評価方針（dialectic で機械スコアをどう扱うか）を確定する。
2. **【第 2 位】結論の信頼性修復 — v2 の P2 (`unsettled`) → P1 (打ち切り明示) → P3 (崩れた論拠遮断＋批准分母修正＋結論入力刷新)**
   - **対象**: `native/src/flow/dialectic/store.{hpp,cpp}`, `exchange.cpp`, `exchange_runner.cpp`, `goal.cpp`, `synthesis.cpp`, `conclusion.cpp`, `native/src/flow/conclusion.cpp`
   - **理由**: 議論エンジンの存在意義である「やり取りと結論の一致」を回復する本丸。実装時は v2 設計書の穴（**批准の過半数分母を残存根拠のみに改める**、**`argumentDepth` 既定は 5 ではなく 3〜4 ＋ `unsettled` で検証する**、**結論 LLM 入力から末尾 20 生発話と旧チャット文言を外し決着表を直接埋め込む**）を必ず塞ぐ。
3. **【第 3 位】`opinion` 枠の死蔵解消とペルソナ・ゲームアーキタイプ文脈の配線（A1・A2 先行、Histrio A3 は別段）**
   - **対象**: `native/src/flow/personas.cpp`, `persona_pool.cpp`, `persona_setup.cpp`, `dialectic/driver.cpp`, `dialectic/position.cpp`, `dialectic/exchange_prompt.cpp`, `flow_setup.cpp`, `paper_markdown.cpp`
   - **理由**: 現在 dialectic で `roster.discussants` 先頭の最上位モデルが 0 発言の `opinion` 枠に消え、全 debater が同一テンプレ性格のまま話している。dialectic では全議論者を `debater`（または役割を再定義）にして多様化したプール人物像 (`persona_descriptor`) を `user` 側へ、ゲームの型（`# ゲームの型`）を `system` 固定側へ配線し、`persona_setup` にも人物像を渡すことで発言の質と多様性が一気に改善する（代理モードは除外維持）。
4. **【第 4 位】論拠応酬・検証可能性の底上げ（P4 声の固定番号＋一覧、P5 `definition` 型＋`qualifier` 入力、P6 元反論者優先、P7 批准結果露出）**
   - **対象**: `native/src/flow/paper_voices.cpp`, `dialectic/issue_voices.cpp`, `dialectic/tension.cpp`, `dialectic/store.cpp`, `dialectic/exchange_runner.cpp`, `native/src/visualize/conclusion_markdown.cpp`
   - **理由**: 外部の声の引用を事後検証可能にし、定義のずれを事実対立と誤分類する問題と再反論の論旨乗り換えを解消する。事実照会も `VoiceCache`＋固定番号空間へ統一する。
5. **【第 5 位】運用堅牢化と計測型収束の整理（P11 CLI エラー可視化＋引用改善、ラベルパーサの単語境界修正、収束ロジック修正、DB 削除漏れ修正）**
   - **対象**: `native/src/persona_engine/llm/{claude_cli,codex_cli,pricing}.cpp`, `native/src/flow/dialectic/{rule_check,position_stance,tension,scheduler,driver}.cpp`, `native/src/flow/discussion_paper.cpp`
   - **理由**: 小粒だが本番障害調査・誤判定・孤立データに直結する欠陥群。`ArgumentGraph` の論点単位リセット（または論点スコープ計測）と `synthesize` の claim カウント除外により計測型収束の破綻を直すか、あるいは投票廃止と合わせて収束仕様そのものを簡素化する。
