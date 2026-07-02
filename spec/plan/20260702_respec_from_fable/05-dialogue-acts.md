# 05 — 会話行為 (dialogue acts) と論証グラフ

対応指摘: **B1** の基盤 (発話を状態を変える「手」にする)。フェーズ: **PR-B**。
正本 spec: [dialectic.md §1, §3](../../feature/flow/dialectic.md)。

## 問題

発話が自由テキストのみで、ターン間に持ち越される状態が生テキストの羅列しかない。
「誰がどの主張に反論し、何が未応答か」を機械が知る手段がなく、止揚判定・収束判定が
テキストの雰囲気判定になっている。

## 設計

### 1. 発話の構造化出力

dialectic エンジン (`flow.engine="dialectic"`) の全ペルソナターンは、露出用の口語文と
論証上の「手」を同時に返す:

```json
{
  "act": "claim | support | rebut | concede | question | synthesize",
  "target": "<応答先 utterance id。claim/question は null 可>",
  "text": "<Discord/Web に流す口語 1〜3 文>"
}
```

- プロンプトに「あなたの発言は上記 JSON で返す。act は今の議論状態で最も議論を前に進める
  手を選ぶ」を追加。act の選択肢はスケジューラが**指名時に制限**できる
  (例: 応答権指名なら `rebut | concede | support` のみ許可 — [06](./06-dialectic-core.md))。
- **パース失敗は degrade**: JSON が取れなければ全文を `act="claim", target=null` として
  受理 + warn (発話を捨てない。OVERVIEW §10)。
- 露出面 (Discord webhook / Web ポーリング) には `text` のみ流す (見た目は現行と同じ)。

### 2. スキーマ (migration `flow_0015`)

```sql
ALTER TABLE flow_utterance ADD COLUMN act TEXT;        -- null = 旧データ/rounds エンジン
ALTER TABLE flow_utterance ADD COLUMN target_id TEXT;  -- 応答先 utterance id
```

- 旧行 (act=null) は読み出し側で `claim` 扱い。rounds エンジンの新規発話は null のまま
  (acts は dialectic エンジン専用。rounds への後付けはしない)。

### 3. 論証グラフの導出 (コード・LLM 不使用)

`src/flow/argument-graph.ts` (新規、純関数 + 小さな状態クラス):

```ts
interface ArgumentGraph {
  apply(u: { id; personaId; act; targetId }): void;  // 状態遷移
  unresolvedRebuttals(): RebuttalEdge[];  // rebut されて defend も concede もされていない辺
  unansweredClaims(): string[];           // 誰も触れていない claim
  concessions(): ConcessionEdge[];        // concede 済みの辺 (止揚の原材料)
  newClaimsSince(turn: number): number;   // 新規論点の枯渇判定用
}
```

遷移規則 (ground/claim 単位):

```
claim ── rebut ──> challenged ── support(同陣営, target=rebut) ──> defended
                              └─ concede(被反論側) ─────────────> conceded
```

- `support` は target が自陣営の claim なら補強、相手の rebut への応答なら防御。
- graph はセッションメモリで構築し、収束判定・スケジューラ・批准入力の 3 箇所が読む。
  永続は flow_utterance (act/target) からいつでも再構築可能 (別テーブル不要)。

## 実装ファイル

| ファイル | 変更 |
|---|---|
| `src/flow/argument-graph.ts` (新規) | グラフ構築・状態遷移・計測 (純関数中心、LLM なし) |
| `src/flow/dialectic/turn-prompt.ts` (新規) | acts 付き発話プロンプト + パース (degrade 込み) |
| `src/flow/director.ts` (dialectic 経路) | 発話 JSON の受理・graph.apply・persistUtterance へ act/target |
| `src/flow/db/migrations.ts` | `flow_0015` |

## テスト / 受け入れ基準

- [ ] act/target が flow_utterance に永続され、旧行 (null) 読み出しが claim 扱いで壊れない
- [ ] JSON 崩れ応答 → act=claim で受理 + warn (発話が落ちない)
- [ ] グラフ遷移: rebut→challenged / support(防御)→defended / concede→conceded が単体テストで決定的
- [ ] `unresolvedRebuttals` / `newClaimsSince` が手作りシーケンスで期待値どおり
- [ ] 新規 `tests/flow/argument-graph.test.ts` / `tests/flow/turn-prompt.test.ts`
