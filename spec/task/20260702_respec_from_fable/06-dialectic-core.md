# 06 — 弁証法コア: Issue/Position/Tension/Synthesis + 批准 + 計測型収束

対応指摘: **B1** (止揚が判定者の創作)・**B2** (対立の型の欠如)・**B3** (折衷と止揚の混同)・
**A6** (収束の計測化)・**A13** (証拠のオンデマンド RAG)。フェーズ: **PR-B**。
正本 spec: [dialectic.md](../../feature/flow/dialectic.md) — 本書はその実装設計。

## 全体像

`flow.engine = "dialectic" | "rounds"` (config、既定 `rounds`) を導入し、dialectic 選択時は
`runDialecticFlow` (新規) が discussion フローを駆動する。rounds エンジン (`runFlow`) は
無改変で併存。dispatch (`dispatch.ts`) が engine 設定を見て分岐する。

位相アルゴリズム・LLM 3 役分離・批准・折衷ゲートは dialectic.md §3〜§5 に従う。

## 1. スキーマ (migration `flow_0016`)

```sql
CREATE TABLE flow_issue (
  id TEXT PRIMARY KEY, session_id TEXT NOT NULL,
  title TEXT NOT NULL, ordinal INTEGER NOT NULL,
  source TEXT NOT NULL DEFAULT 'llm',       -- llm / paper-gate (09 の前倒し分解)
  status TEXT NOT NULL DEFAULT 'open',      -- open / concluded
  created_at INTEGER NOT NULL
);
CREATE TABLE flow_position (
  id TEXT PRIMARY KEY, issue_id TEXT NOT NULL, persona_id TEXT NOT NULL,
  stance TEXT NOT NULL,                     -- pro / con
  claim TEXT NOT NULL,
  grounds_json TEXT NOT NULL,               -- [{id, text, state}] state: unchallenged/challenged/defended/conceded
  values_json TEXT NOT NULL,                -- string[]
  created_at INTEGER NOT NULL
);
CREATE TABLE flow_tension (
  id TEXT PRIMARY KEY, issue_id TEXT NOT NULL,
  position_a TEXT NOT NULL, position_b TEXT NOT NULL,
  type TEXT NOT NULL,                        -- facts / values / means / tradeoff
  status TEXT NOT NULL DEFAULT 'open',       -- open / resolved(事実解消) / synthesized / compromised / agreed_disagree / unresolved_fact
  resolution_note TEXT,                      -- 事実解消の内容 or 未解決事実問題の記述
  created_at INTEGER NOT NULL
);
CREATE TABLE flow_synthesis (
  id TEXT PRIMARY KEY, tension_id TEXT NOT NULL,
  preserves_json TEXT NOT NULL,              -- ground id[]
  negates_json TEXT NOT NULL,
  elevates TEXT,                             -- null/空 = 折衷
  kind TEXT NOT NULL,                        -- aufheben / compromise (elevation ゲート結果)
  text TEXT NOT NULL,                        -- 露出用の統合文
  ratification_json TEXT NOT NULL,           -- [{positionId, verdict: accept/reject, preservedIds[], missing[], reason}]
  status TEXT NOT NULL,                      -- ratified / rejected / revising
  revision INTEGER NOT NULL DEFAULT 0,       -- 修正ループ回数 (最大 2)
  created_at INTEGER NOT NULL
);
```

## 2. 位相ドライバ (`src/flow/dialectic/driver.ts`)

```
[0] 論点分解:  paperOverride.issues があれば採用 (09 のゲート前倒し)。
              無ければ decomposeIssues(theme, paper) を LLM 生成 → 3〜5 個にクランプ。
[1] 定立:     issue × debater(pro/con 各代表) に Position 生成 (claim + grounds 2〜4 + values)。
              01 の coreClaims を種にする。
[2] 反定立:   スケジューラが応答権指名 (acts 制限 = rebut/support/concede/question)。
              graph (05) 更新。issue あたり反定立ターン上限 = flow.dialectic.rebutTurns (既定 4)。
[3] 矛盾特定: openな対立 (challenged で defend/concede が済んだ組) を classifyTension (小モデル・温度0) で型分類。
              facts → resolveFactTension: listRelevantExternalVoices で証拠を引き、
              判定 LLM が「証拠は A/B/どちらも支持しない」を出す。
                支持 → status=resolved + resolution_note / 判定不能 → status=unresolved_fact (結論に明記)
[4] 止揚:     values/means/tradeoff の open tension に型別テンプレートで synthesize (生成 LLM)。
              elevationGate (小モデル): elevates が実質空/「両方やる」→ kind=compromise。
              ratify: 両 Position の持ち主ペルソナへチェックリスト批准 (reject デフォルト、§spec 5-2)。
                両 accept → status=ratified (+ tension=synthesized or compromised)
                reject あり → missing を渡して修正 (revision ≤ 2)、尽きたら status=rejected → tension=agreed_disagree
[5] 収束:     全 issue で open tension が無くなり次第 concludeDialectic へ。
              または計測型シグナル 2/3 (下記) で早期終了。
```

### 計測型収束 (dialectic.md §6)

コードのみで判定 (`src/flow/dialectic/convergence.ts`):

- (a) `graph.unresolvedRebuttals().length === 0`
- (b) レンズ投票の `winnerShare >= flow.convergeShare` (03/04 を流用。dialectic では issue 決着ごとに投票)
- (c) `graph.newClaimsSince(now - K) === 0` (K = `flow.dialectic.staleTurns` 既定 6)

2/3 成立で収束。`facilitator.aufhebungTarget` は「ratified aufheben 数の上限キャップ」として
のみ参照 (到達で必ず収束)。

### 結論

`concludeDialectic` が converge プロンプトへ渡す (04 の拡張をさらに置換):

- issue ごとの決着表 (Position 対 → ratified Synthesis / compromise / agreed_disagree)
- **未解決の事実問題** (unresolved_fact の resolution_note) — 結論本文に必ず明記
- レンズ投票の集中度

## 3. スケジューラ (`src/flow/dialectic/scheduler.ts`)

決定的コード。優先度: ①未応答 rebut への応答権 (被反論者を指名) → ②未分類 tension の攻撃番
(接続数最大の issue から) → ③発言数最少ペルソナの自由ターン。
ファシリテーターの露出文はレンダリングのみ LLM (「凛の『…』にまだ応答がありません。翔さん、
賛成側としてどう応えますか」)。withCostLog location="facilitator"。

## 4. LLM 呼び出し一覧 (すべて withCostLog)

| location | 役割 | モデル | 温度 |
|---|---|---|---|
| issue-decompose | [0] 論点分解 | メイン | 中 |
| position | [1] 定立 | メイン | 高め |
| utterance | [2] 反定立ターン (05 の acts 付き) | メイン | 高め |
| tension-classify | [3] 型分類 | `flow.dialectic.judgeModel` (既定 Haiku 級) | 0 |
| fact-resolve | [3] 事実照会判定 | judgeModel | 0 |
| synthesize | [4] 止揚生成 (型別テンプレ) | メイン | 高め |
| elevation-gate | [4] 折衷判定 | judgeModel | 0 |
| ratify | [4] 批准 ×2 | judgeModel | 0 |
| facilitator | 指名文レンダリング | judgeModel | 低 |

## 実装ファイル

`src/flow/dialectic/` に新設 (rounds エンジンとディレクトリで分離):
`driver.ts` / `scheduler.ts` / `issues.ts` (分解+クランプ) / `position.ts` / `tension.ts`
(分類+事実解消) / `synthesis.ts` (生成+ゲート+批准+修正ループ) / `convergence.ts` /
`conclusion.ts` / `store.ts` (4 表の永続)。既存流用: 05 の argument-graph / turn-prompt、
03 の vote、01 の persona-setup。`dispatch.ts` に engine 分岐、`config.ts` に
`flow.engine` / `flow.dialectic.{rebutTurns, staleTurns, judgeModel}`。

## テスト / 受け入れ基準

- [ ] MockLLM で位相 [0]→[5] が完走し、4 表に整合したレコードが残る (E2E 相当の `tests/flow/dialectic-driver.test.ts`)
- [ ] 事実対立: Mock 証拠で resolved、証拠なしで unresolved_fact になり結論文に「未解決の事実問題」が含まれる
- [ ] 批准: 片方 reject → 修正ループ → 2 回で打ち切り agreed_disagree
- [ ] elevates 空の Synthesis が kind=compromise になり早期収束シグナルに数えられない
- [ ] `flow.engine=rounds` (既定) で既存挙動が完全無変化 (既存全テスト通過)
- [ ] スケジューラ: 未応答 rebut があるとき被反論者が指名される (決定的単体テスト)
