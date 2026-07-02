# 20260702 議論エンジン再設計 (respec from Fable セッションレビュー)

> 2026-07-02 の Fable セッションで行った議論エンジン (`src/flow/`) の正直レビューで
> 合意した全指摘を解決するための spec 改訂 + 実装設計書。
> 正本 spec: [`spec/feature/flow/dialectic.md`](../../feature/flow/dialectic.md) (新規) +
> [`discussion.md`](../../feature/flow/discussion.md) / [`OVERVIEW.md`](../../feature/flow/OVERVIEW.md) /
> [`improvement.md`](../../feature/flow/improvement.md) (改訂)。

## 指摘 → 設計書 対応表

| # | セッションでの指摘 | 設計書 |
|---|---|---|
| A1 | debater の stance が毎ターン再抽選され人格に一貫した立場がない | [01](./01-persona-state.md) |
| A2 | 発言者がランダム抽選で発言機会が保証されない | [02](./02-turn-scheduling.md) |
| A3 | 改善フロー機械スコアのカテゴリエラー (文面の感情 ≠ 採用効果) | [07](./07-improvement-scoring.md) |
| A4 | 投票が N 人分のコストで実質 1 票 (同一プロンプト・順序バイアス・番号回答不受理) | [03](./03-vote-lenses.md) |
| A5 | 結論生成がラウンドサマリを使っていない | [04](./04-conclusion-convergence.md) |
| A6 | 早期収束の唯一の条件が「止揚の個数」= 実質ラウンド数 | [04](./04-conclusion-convergence.md), [06](./06-dialectic-core.md) |
| A7 | concluded 判定の二重基準 (JSON パース失敗でも concluded=true) | [04](./04-conclusion-convergence.md) |
| A8 | ファシリテーター開幕ターンに文脈がない | [02](./02-turn-scheduling.md) |
| A9 | ペルソナ多様性が化粧的 (同一モデル × 3 風味) | [01](./01-persona-state.md) |
| A10 | improvement の射影スコアを投票数として表示層に流している | [03](./03-vote-lenses.md) |
| A11 | runFlow が 440 行の単機能巨大関数 (SRP 違反) | [10](./10-code-hygiene.md) |
| A12 | プロンプト組み立ての重複 (paperToPrompt / buildPersonaUserPrompt) | [10](./10-code-hygiene.md) |
| A13 | 論争当事者 (debater) だけが証拠を持たない | [08](./08-paper-provenance.md), [06](./06-dialectic-core.md) |
| B1 | 止揚が「判定者の創作」になる構造 (生成兼判定・検証不能) | [05](./05-dialogue-acts.md), [06](./06-dialectic-core.md) |
| B2 | 対立の型 (事実/価値/手段/トレードオフ) の区別がない | [06](./06-dialectic-core.md) |
| B3 | 折衷と止揚の区別がない | [06](./06-dialectic-core.md) |
| C1 | LLM 増補メカニクスが出所ラベルなしで「事実」として混入 | [08](./08-paper-provenance.md) |
| C2 | ペーパー評価に「議論適性」軸が欠落 (争点存在/両論武装/証拠バランス) | [09](./09-paper-gate-debatability.md) |
| C3 | 論点分解が人間レビューの管轄外 | [09](./09-paper-gate-debatability.md) |

## フェーズ計画 (実装単位)

依存が薄い順に 4 フェーズ。**各フェーズ = 1 PR** (AI 実装 1 PR 集約ルール準拠)。

| フェーズ | 設計書 | 内容 | migration |
|---|---|---|---|
| **PR-A: 即効修正** | 01, 02, 03, 04, 10 | stance 固定・価値軸 / シャッフル輪番 + ファシリ文脈 / レンズ投票 / 結論入力 + concluded 統一 / 分割・重複解消 | flow_session_persona 1 本 |
| **PR-B: 論証状態機械** | 05, 06 | dialogue acts / Issue-Position-Tension-Synthesis / 批准 / 計測型収束。`flow.engine="dialectic"` opt-in | act 列 + 弁証法 4 表 |
| **PR-C: 改善フロー再設計** | 07 | 効果予測ベクトルによるスコアリング | なし |
| **PR-D: ペーパー再設計** | 08, 09 | 出所ラベル / 仮説メカニクス分離 / 議論適性ゲート / 論点分解の前倒し | mechanics JSON 内 source 追加 (migration 不要)、issue 節 |

- PR-A は既存 rounds エンジンの品質を直接引き上げる (dialectic 移行後も無駄にならない —
  stance 固定は信念状態の、輪番は指名制の下位互換)。
- PR-B は `flow.engine` config スイッチで opt-in。既定は `rounds` のまま、検証後に切替。
- PR-C / PR-D は互いに独立。PR-A とも独立して着手可。

## 共通実装ルール

- 変更はすべて `src/flow/` 配下。coding-conventions (SRP・ファイル分割) 準拠。
- **全 LLM 呼び出しは `withCostLog` 経由** (既存規律の維持)。
- **失敗は握り潰さない** (OVERVIEW §10): degrade するときは warn + 発話/ログに明示。
- 各設計書に受け入れ基準とテスト方針を記載。テストは既存の `npm run test:*` 群に倣い
  `tests/` に追加、LLM は Mock 注入 (既存 DI 境界を使う)。
- 後方互換: 既存 DB 行 (act 列なし・source なし mechanics) を読めること。
