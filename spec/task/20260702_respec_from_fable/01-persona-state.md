# 01 — ペルソナ状態: stance セッション固定 + 価値軸生成

対応指摘: **A1** (stance 毎ターン再抽選)・**A9** (多様性が化粧的)。フェーズ: **PR-A**。

## 問題

- `personas.ts:76-80` `decideStance` が debater の pro/con を**ターンごとに 50:50 再抽選**する。
  同一ペルソナがターン 2 で賛成・ターン 5 で反対し、立場の一貫性がない。議論が積み上がらず、
  止揚判定の前提 (対立軸の持続) が崩れている。
- 全ペルソナが同一モデル・ロール別固定 traits 3 種 (`ROLE_FLAVOR`)。「N 人の議論」の実体が
  1 モデル × 3 プロンプト風味で、意見の多様性が温度サンプリング頼み。

## 設計

### 1. stance はセッション開始時に固定

- `generateFlowPersonas` で debater 生成時に stance を確定して `FlowPersona.stance` に持たせる。
  debater 数を pro/con に**均等割** (奇数なら rng で余り 1 を振る)。
- `decideStance(persona, rng)` は廃止し、`personaStance(persona): FlowStance` (純関数、
  persona.stance / role から導出) に置換。facilitator=neutral / opinion=opinion は従来通り。
- `FlowUtteranceRecord.stance` の記録・表示 (`persona-display.ts` の `composeDisplayName`) は
  既存のまま動く (値がターン間で安定するだけ)。

### 2. 価値軸 + 核主張の生成 (セッション 1 回の LLM 呼び出し)

- セッション開始時 (ペルソナ生成直後) に **1 回だけ** LLM を呼び、全ペルソナぶんの
  「価値軸 + 核主張」をまとめて生成する:

```
入力: テーマ + ペルソナ一覧 (name/role/stance)
出力 (構造化 JSON): [{ personaId, valueAxis: "収益性より長期のプレイヤー信頼を重視",
                       coreClaims: ["...", "..."] (debater のみ 1〜2 個) }]
```

- `FlowPersona` に `valueAxis?: string` / `coreClaims?: string[]` を追加。
- `buildPersonaUserPrompt` のペルソナ行に価値軸を注入:
  `あなたが重視する価値: {valueAxis}`。coreClaims は debater の stance 行直後に
  `あなたの核となる主張: ...` として注入。
- LLM 失敗時は valueAxis なしで degrade (warn。議論は止めない)。
- 生成コスト: セッションあたり +1 call (小モデル可、`cfg.flow.personaSetupModel` 追加・
  既定は空 = メインモデル)。

### 3. 永続化: flow_session_persona

議論の再現性・レビュー可能性のため、セッションのキャストを永続する (migration `flow_0014`):

```sql
CREATE TABLE flow_session_persona (
  id TEXT PRIMARY KEY,            -- persona_id (randomUUID)
  session_id TEXT NOT NULL,
  name TEXT NOT NULL,
  role TEXT NOT NULL,             -- facilitator / debater / opinion
  stance TEXT NOT NULL,           -- pro / con / neutral / opinion (セッション固定値)
  value_axis TEXT,
  core_claims_json TEXT,          -- string[]
  possession_name TEXT,           -- 憑依対象 (あれば)
  created_at INTEGER NOT NULL
);
CREATE INDEX idx_fsp_session ON flow_session_persona(session_id);
```

PR-B (信念状態) はこの表に `belief_json` を足して育てる前提の置き場所。

## 実装ファイル

| ファイル | 変更 |
|---|---|
| `src/flow/personas.ts` | `FlowPersona.stance/valueAxis/coreClaims` 追加、均等割、`decideStance` → `personaStance` |
| `src/flow/persona-setup.ts` (新規) | 価値軸/核主張の一括生成 (withCostLog, location="persona-setup") + flow_session_persona 永続 |
| `src/flow/discussion-paper.ts` | `buildPersonaUserPrompt` に価値軸/核主張ブロック注入 |
| `src/flow/director.ts` | セッション初期化で persona-setup 呼び出し、ターン内 `decideStance` 呼びを置換 |
| `src/flow/db/migrations.ts` | `flow_0014` (表追加) |

## テスト / 受け入れ基準

- [ ] 同一セッション内で同一 persona の stance が全発話で不変 (`flow_utterance` を検証)
- [ ] debater 4 人 → pro 2 / con 2、5 人 → 3:2 or 2:3 (rng 固定で決定的)
- [ ] 価値軸 LLM 失敗時: warn が出て議論は完走、valueAxis 無しの prompt になる
- [ ] flow_session_persona にキャスト全員が永続される (可視化・後続 PR-B が参照可能)
- [ ] 既存テスト (`npm run test:phase5` 等) が通る (stance 型の互換維持)
