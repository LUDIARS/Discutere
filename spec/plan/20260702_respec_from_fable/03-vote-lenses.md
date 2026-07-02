# 03 — 投票再設計: 評価レンズ + 順序シャッフル + tally 意味分離

対応指摘: **A4** (N 人同一プロンプトで実質 1 票・位置バイアス・番号回答不受理)・
**A10** (improvement の射影スコアを票数として表示)。フェーズ: **PR-A**。

## 問題

- `vote.ts:75-98` が voterCount 回、完全同一のプロンプトを同一モデルへ投げる。N 票は強相関で
  「世論」の分散は温度ノイズのみ。候補提示順も全投票者で同一 → 位置バイアスがそのまま集計に乗る。
- プロンプトが候補に番号を振るため「1」と答えるモデル回答が `parseVoteId` で棄権扱いになる。
- 同点は先着 (`vote.ts:107-115`) で早い発話に有利。
- `improvement.ts:82-84` が生の射影スコア (負値・小数) を `VoteResult.tally` に詰め、
  `onVote` 経由で Discord の「得票」リアクション表示に流れる (票数前提の表示層と意味が乾いていない)。

## 設計

### 1. 評価レンズ別投票

- 投票者 i にレンズ (評価観点) を割り当てる。既定レンズ:

```ts
const VOTE_LENSES = [
  { key: "logic",     instruction: "論理の一貫性・飛躍のなさを最重視して選ぶ" },
  { key: "grounds",   instruction: "根拠・具体例の裏付けを最重視して選ぶ" },
  { key: "relevance", instruction: "テーマへの適合・議論を前に進める度合いを最重視して選ぶ" },
];
```

- `voterCount` 人をレンズに巡回割当 (voterCount=3 なら 1 人ずつ、6 なら 2 人ずつ)。
  レンズは config `flow.voteLenses[]` で上書き可 (既定は上記 3 種)。
- 投票プロンプトの「中立な審判として」の後にレンズ instruction を差し込む。
  ペルソナは従来通り注入しない (中立性は維持、**観点だけ**分ける)。

### 2. 候補順シャッフル + 番号回答受理

- 投票者ごとに候補列を rng シャッフルして提示する (位置バイアスの脱相関)。
- `parseVoteId` を拡張: ID 完全一致 / 部分一致に加え、**提示順の番号回答**
  (`"2"` / `"2."` / `"2 番"` 等、先頭の整数) を受理し、その投票者に提示した順の
  候補へ解決する。
- 同点処理: 先着 → **rng による同点くじ**に変更 (早い発話へのバイアス除去。
  rng 注入で決定的にテスト可能)。

### 3. tally の意味分離

`VoteResult` に種別を持たせ、表示層が票数とスコアを区別できるようにする:

```ts
export interface VoteResult {
  kind: "votes" | "scores";   // 追加。既定 "votes"
  tally: Record<string, number>;
  winner: string | null;
}
```

- `runRoundVote` は `kind: "votes"`、improvement の機械スコア evaluator は `kind: "scores"`。
- `VoteEvent` にも `kind` を伝搬。`discord-live` / `slack-live` の得票可視化は
  `scores` のとき「得票 n 票」ではなく「スコア x.xx (design_gap 適合)」表記 + 🏆 のみ
  (👍 の枚数積みはしない)。

## 実装ファイル

| ファイル | 変更 |
|---|---|
| `src/flow/vote.ts` | レンズ割当・シャッフル・番号受理・同点くじ・`kind` |
| `src/flow/director.ts` | `VoteEvent.kind` 伝搬 |
| `src/flow/improvement.ts` | evaluator が `kind: "scores"` を返す |
| `src/flow/discord-live.ts` / `src/slack/slack-live.ts` | scores 表示分岐 |
| `src/config.ts` | `flow.voteLenses[]` (既定 3 種) |

## テスト / 受け入れ基準

- [ ] voterCount=3 で 3 投票者のプロンプトが相互に異なる (レンズ行 + 候補順)
- [ ] `parseVoteId("2", ...)` が提示順 2 番目の候補 ID に解決される
- [ ] 同点時、rng 固定で winner が決定的、かつ提示順に依存しない
- [ ] improvement 経路の `onVote` が `kind: "scores"` を受け、Discord 表示が票数表記にならない
- [ ] `npm run test:queue` / 既存 vote 関連テスト通過 + 新規 `tests/flow/vote-lenses.test.ts`
