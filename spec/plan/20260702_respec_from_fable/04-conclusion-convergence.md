# 04 — 結論生成の入力拡充 + concluded 判定統一 + 収束シグナル

対応指摘: **A5** (結論がラウンドサマリを使わない)・**A7** (concluded 二重基準)・
**A6** (早期収束 = 止揚件数、の暫定緩和)。フェーズ: **PR-A**
(計測型収束の本体は PR-B [06](./06-dialectic-core.md))。

## 問題

- `conclusion.ts:68-77`: converge プロンプトへの入力が「直近 20 発話 + 止揚 + winner 発話」
  のみ。毎ラウンド生成しているサマリ (`summarizeRound`) が結論に渡っておらず、
  序盤ラウンドの内容は止揚以外すべて落ちる。
- `conclusion.ts:93-106`: JSON パース成功なら parsed.summary、**パース失敗でも生テキストを
  結論として concluded=true**。一方、止揚判定はパース失敗を無言で「止揚なし」扱い
  (`round-summary.ts:115-117`) — 同種の失敗への厳密さが不統一。
- 早期収束 (`director.ts:570`) が `allAufhebung.length >= aufhebungTarget` のみ。
  止揚はラウンドあたり最大 1 件なので実質「N ラウンド経過」と等価。

## 設計

### 1. 結論入力の拡充

`generateConclusion` の引数に `rounds: RoundSummary[]` を追加し、converge プロンプトへ
`# 各ラウンドの要約` 節として全ラウンドサマリを渡す (director は `paper.rounds` を保持済み)。
`recentAll` (直近 20 発話) は「終盤の生の空気」として残す。

### 2. concluded 判定の統一

方針: **構造化出力の失敗は 1 回だけリトライし、それでも失敗なら degrade を明示する**。

- 結論: `extractJsonObject` 失敗 → **1 回リトライ** (同一プロンプト) → なお失敗なら
  生テキストを `summary` に入れるが **`concluded=false`** とし、summary 冒頭に
  `【収束(未検証)】` プレフィックスを付ける (表示層で区別可能)。warn 必須。
- 止揚判定: パース失敗を無言で捨てず **warn を出す** (挙動は「止揚なし」のまま)。
- リトライも `withCostLog` 経由 (location="summary"、コスト可視)。

### 3. 収束シグナルの暫定拡張 (PR-A の範囲)

計測型収束 (未応答反論数 等) は論証グラフ (PR-B) が前提のため、PR-A では
**投票集中度**のみ先行導入する:

- ラウンド投票の最多得票シェア `winnerShare = maxVotes / totalValidVotes` を
  `VoteResult` に追加 (03 の `kind` と同時変更)。
- 早期収束条件を「止揚 ≥ aufhebungTarget **かつ** 直近ラウンドの winnerShare ≥
  `flow.convergeShare` (既定 0.6)」の AND に変更。片方だけでは打ち切らない。
  - 世論が割れているのに止揚件数だけで打ち切る現行の空洞化を防ぐ。
  - improvement (scores) は winnerShare を計算できないため従来条件のまま。

## 実装ファイル

| ファイル | 変更 |
|---|---|
| `src/flow/conclusion.ts` | rounds 入力・リトライ・concluded 判定統一 |
| `src/flow/round-summary.ts` | 止揚パース失敗の warn |
| `src/flow/vote.ts` | `winnerShare` 算出 |
| `src/flow/director.ts` | generateConclusion へ rounds 渡し、早期収束条件 AND 化 |
| `src/config.ts` | `flow.convergeShare` (既定 0.6) |

## テスト / 受け入れ基準

- [ ] converge プロンプトに全ラウンドサマリが含まれる (プロンプト組み立て単体テスト)
- [ ] Mock LLM が壊れた JSON を 2 回返すと `concluded=false` + `【収束(未検証)】` + warn
- [ ] Mock LLM が 1 回目失敗 → 2 回目成功で `concluded=true` (リトライ動作)
- [ ] 止揚 JSON 失敗時に warn が出る
- [ ] 止揚件数到達 + winnerShare 0.4 では打ち切られない / 0.7 なら打ち切られる
- [ ] `npm run test:phase5` 通過
