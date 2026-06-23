# persona-engine (Phase 0)

「ペルソナ × チャットルール × LLM 呼び出し」 で **議論を自走させる** 汎用エンジン。
Concordia の persona / rules 機構を起点にし、 議論 (Discatier Core) 向けに
独自進化させた版。

将来 **別パッケージ (`@ludiars/persona-engine` 等) として切り出す** ことを前提に、
`src/persona-engine/` 配下に閉じた構造で実装する。 切り出し時は
`mv src/persona-engine ../persona-engine-package/src/` + `package.json` 分割の
機械作業のみで完結することを設計ゴールとする。

## 切り出し条件 (Phase 0 で守る境界)

1. **Discutere 固有 import を持たない** — `src/core/*` 等を直接 import しない
2. **外部依存は interface で inject** — LLMClient / DiscussionContextProvider /
   Logger / DB instance はすべて consumer から渡す
3. **public API は 1 関数** — `createPersonaEngine({...})` のみが top-level export、
   内部 (PersonasRepo / RulesRepo / engine 等) は engine handle 経由で操作
4. **DB migration は self-contained** — `applyPersonaEngineMigrations(db)` を
   呼べば persona-engine が必要な table をすべて作る
5. **glue 層は別ディレクトリ** — Discatier 接続は `src/discatier-engine-adapter/`
   に隔離、 ここに persona-engine の internal を import しない

## アーキテクチャ

```
                    consumer (Discutere / Concordia / 他)
                                 │
                                 ▼
                     ┌──────────────────────┐
                     │ createPersonaEngine  │  ← Phase 0 の唯一 public 関数
                     └──────────┬───────────┘
                                │ DI: { db, llm, contextProvider, logger, options }
                ┌───────────────┼───────────────────────┐
                ▼               ▼                       ▼
       ┌──────────────┐  ┌─────────────┐       ┌────────────────┐
       │ PersonasRepo │  │ RulesRepo   │       │ rules engine   │
       │ + seeds      │  │ + log       │       │ + prompt build │
       └──────────────┘  └─────────────┘       │ + handler      │
                                                └───────┬────────┘
                                                        │
                                                        ▼
                                                ┌───────────────┐
                                                │   LLMClient   │ (interface)
                                                │ Anthropic SDK │
                                                │ Mock (tests)  │
                                                │ Codex CLI ... │
                                                └───────────────┘
```

## public API (Phase 0)

```ts
import {
  createPersonaEngine,
  applyPersonaEngineMigrations,
  type LLMClient,
  type DiscussionContextProvider,
  type PersonaSeed,
  type PersonaEngineOptions,
} from "@ludiars/persona-engine"; // 切り出し後の package 名想定
```

`createPersonaEngine` 戻り値:

```ts
interface PersonaEngineHandle {
  /** rule engine の tick を開始 */
  start(): void;
  /** tick を止める (test / shutdown) */
  stop(): void;
  /** persona / rule の CRUD は repo 経由 */
  personas: PersonasRepo;
  rules: RulesRepo;
  /** 1 件だけ手動 fire (テスト / debug) */
  fireRule(ruleId: string, triggeredBy: string): Promise<void>;
  /** runtime kill switch (admin UI から呼ぶ) */
  setRulesEnabled(enabled: boolean): void;
}
```

## 議論特化 persona seed (Phase 0)

Concordia の 10 名 (architect-sensei / test-soul 等) は **そのままでは使わず**、
議論モード専用に 8-10 名を新規設計。 軸:

| ID                | 名前       | 議論ロール |
|-------------------|-----------|----------|
| advocate          | 推進派    | 新仮説を積極的に出す |
| sceptic           | 懐疑者    | 既存仮説の弱点を突く |
| refiner           | 慎重派    | 既存仮説の精緻化を好む、 統合的 |
| neutral-observer  | 中立観察者 | 議論全体の俯瞰、 第三者視点 |
| evidence-seeker   | 検証者    | utterance / 観測データを根拠として要求 |
| integrator        | 統合者    | 複数仮説を 1 つに束ねる |
| historian         | 歴史尊重派 | 過去議論を参照、 棄却済 hypothesis を持ち出す |
| innovator         | 革新者    | 既存枠組みを壊す方向の提案 |
| (option) jester   | 風刺者    | 議論の真面目さを和らげる比喩、 緊張緩和役 |
| (option) silent-witness | 沈黙の証人 | 滅多に発話しないが、 する時は核心 |

各 seed は `traits[]` と `speech_style` を持ち、 prompt-builder で LLM に渡す。

## rules (Phase 0 サンプル)

議論を駆動する rule の初期セット:

| id                | trigger     | 主目的 |
|-------------------|-------------|--------|
| propose-on-gap    | event:GapDetected | gap が検出されたら advocate が hypothesis を出す |
| refute-cold       | tick=300s   | 5 分間反論が無い hypothesis を sceptic が突く |
| refine-validated  | event:HypothesisValidated | validated hypothesis を refiner が精緻化 |
| integrate-on-many | tick=600s   | 同 gap に 3+ hypothesis が並んだら integrator が統合提案 |

各 rule は cooldown_sec を持ち、 同 persona / 同 rule の連発を防ぐ。
runtime kill switch (rulesDisabled) で全体停止可能。

## LLMClient interface

```ts
interface LLMClient {
  /** prompt → 1 件の応答テキスト (JSON 期待) */
  invoke(args: {
    prompt: string;
    model?: string;
    maxTokens?: number;
    /** persona の speech_style を反映するためのシステムプロンプト */
    system?: string;
  }): Promise<{ ok: true; text: string; usage?: TokenUsage } | { ok: false; error: string }>;
}
```

実装:
- `AnthropicSdkClient` — `@anthropic-ai/sdk` (env `ANTHROPIC_API_KEY`)、 model default `claude-haiku-4-5-20251001`
- `MockLLMClient` — テスト用、 固定 JSON 返す or scripted response

## DiscussionContextProvider interface

```ts
interface DiscussionContextProvider {
  /** prompt-builder が議論コンテキストを組み立てるための fetcher */
  listActiveHypotheses(workspaceId: string, limit: number): Hypothesis[];
  listRecentGaps(workspaceId: string, limit: number): Gap[];
  listRecentUtterances(workspaceId: string, sessionId: string, limit: number): Utterance[];
  /** persona の handler が「新規 hypothesis を提案」 等の副作用を書き戻す */
  proposeHypothesis(input: ProposeHypothesisInput): { id: string };
  postUtterance(input: PostUtteranceInput): { id: string };
}
```

Phase 0 では Discutere の Discatier Core 用 adapter (`src/discatier-engine-adapter/`)
が唯一の実装。 後で別 consumer (Concordia / 別サービス) が同 interface を満たす
adapter を書けば動く。

## DB schema (`applyPersonaEngineMigrations(db)` で適用)

```sql
CREATE TABLE personas (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  display_name TEXT NOT NULL DEFAULT '',
  description TEXT NOT NULL,
  traits TEXT NOT NULL,            -- JSON
  speech_style TEXT NOT NULL,
  skill_template TEXT NOT NULL DEFAULT '',
  learned_notes TEXT NOT NULL DEFAULT '[]',  -- JSON
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE TABLE persona_assignments (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  persona_id TEXT NOT NULL REFERENCES personas(id),
  session_id TEXT NOT NULL,
  assigned_at INTEGER NOT NULL,
  released_at INTEGER
);

CREATE TABLE persona_feedback (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  persona_id TEXT NOT NULL REFERENCES personas(id),
  session_id TEXT,
  ts INTEGER NOT NULL,
  kind TEXT NOT NULL,               -- session-end / chat-update / manual / system
  delta TEXT NOT NULL,
  detail TEXT
);

CREATE TABLE rules (
  id TEXT PRIMARY KEY,
  description TEXT,
  trigger_type TEXT NOT NULL,        -- tick / event
  tick_sec INTEGER,
  event_kind TEXT,
  conditions TEXT NOT NULL DEFAULT '[]', -- JSON
  instructions TEXT NOT NULL,
  target TEXT,
  cooldown_sec INTEGER NOT NULL DEFAULT 60,
  last_fired_at INTEGER,
  enabled INTEGER NOT NULL DEFAULT 1,
  added_at INTEGER NOT NULL,
  added_by TEXT NOT NULL,
  removed_at INTEGER,
  removed_by TEXT,
  removed_reason TEXT
);

CREATE TABLE rule_log (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts INTEGER NOT NULL,
  rule_id TEXT,
  action TEXT NOT NULL,              -- add / remove / fire / skip / error
  detail TEXT,
  actor TEXT NOT NULL                -- system / ai / human / engine
);
```

table 名はすべて `pe_` prefix を付けずに置く (= 同じ DB に他テーブルが
あっても衝突しない名前) — 後から prefix 付与は migration v2 で扱う。

## Phase ロードマップ

- **Phase 0 (本 PR)** — 上記すべて + Discatier adapter + MockLLM テスト + Anthropic SDK 接続 + 議論用 8-10 名 + 4 種類 rule seed
- **Phase 1** — persona の learned_notes 自動更新 (hypothesis 採用率から)、 統計、 admin UI スタブ
- **Phase 2** — Concordia から persona-engine 移行 (= Concordia と Di が同 engine を共用)、 npm 切り出し
- **Phase 3** — multi-agent 議論 (= 複数 persona が並行発話)、 conflict 解決

## 非ゴール (Phase 0)

- 切り出し PR 自体 (Phase 2)
- learned_notes の自動更新 (Phase 1)
- Concordia 側の移行 (Phase 2)
- multi-agent 並行発話 (Phase 3)
- admin Web UI (Concordia にある runtime kill switch GUI は Phase 1 で持ち込む)

## 参照

- Concordia の起点コード: `LUDIARS/Concordia/src/{personas,rules,db/personas-repo.ts,db/rules-repo.ts}`
- Discatier Core: `src/core/repositories/base.ts`
- visualize との接続: `[[persona:<id>]]` を wikilink に追加 (`spec/feature/visualize/DESIGN.md` 更新)
