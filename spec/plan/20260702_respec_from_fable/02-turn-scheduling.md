# 02 — ターンスケジューリング: シャッフル輪番 + ファシリテーター文脈

対応指摘: **A2** (発言者ランダム抽選)・**A8** (ファシリ開幕に文脈なし)。フェーズ: **PR-A**
(指名制への拡張は PR-B [06](./06-dialectic-core.md))。

## 問題

- `director.ts:423` が毎ターン `pickRandomPersona` (facilitator 含む全員から抽選)。
  同一人物の連続発言・1 ラウンド無発言・pro 不在ラウンドが確率で起きる。
- ラウンド開幕のファシリテータープロンプト (`director.ts:369-372`) が「テーマ + ラウンド番号」
  のみで、前ラウンドの結果を知らない。ラウンドが「同じ議論の N 回試行」になっている。

## 設計

### 1. シャッフル輪番 (round-robin with shuffle)

- ラウンド開始時に **facilitator を除く** ペルソナ列を rng でシャッフルし、ターンは
  その順に巡回する (`turn % speakers.length`)。
  - turnsPerRound ≥ 人数 なら**全員が最低 1 回発言**することを保証。
  - turnsPerRound < 人数 でもシャッフル順の先頭から重複なく充てる。
- facilitator は開幕ターン (turn 0) 専任に変更する。ターンループの抽選対象から外す
  (現行は facilitator も抽選候補で、役割が二重だった)。
  - 中盤介入 (対立整理) は PR-B の指名制ディレクターで正式に導入するため、PR-A では持たない。
- 実装は `src/flow/turn-scheduler.ts` (新規) に純関数で切り出す:
  `buildTurnOrder(personas, turnsPerRound, rng): FlowPersona[]` — テスト容易性のため
  Rng 注入・決定的。

### 2. ファシリテーター開幕に前ラウンド文脈

開幕プロンプトに以下を追加 (round ≥ 2 のとき):

```
# 前ラウンドの結果
{直近ラウンドの summary}
止揚: {当該ラウンドの aufhebung (あれば)}
世論 (最多得票): {winner 発話の personaName + text 先頭 120 字}

# 指示
前ラウンドの到達点を踏まえ、まだ深まっていない論点・対立が残る論点へ
議論を進める出題を 1〜2 文で行ってください。前ラウンドの繰り返しは避ける。
```

- winner text は `allUtterances` から引く (director が既に保持)。
- round 1 は従来通り (文脈なし出題)。

## 実装ファイル

| ファイル | 変更 |
|---|---|
| `src/flow/turn-scheduler.ts` (新規) | `buildTurnOrder` 純関数 |
| `src/flow/director.ts` | 抽選 → 輪番順の消費、開幕プロンプト組み立てに前ラウンド文脈、facilitator をターン対象から除外 |
| `src/flow/personas.ts` | `pickRandomPersona` は sparring 等の既存利用が無ければ削除 (残す場合は deprecated コメント) |

## テスト / 受け入れ基準

- [ ] rng 固定で turn 順が決定的、turnsPerRound=人数 のとき全員 1 回ずつ
- [ ] turnsPerRound > 人数 のとき 2 周目もシャッフル順 (連続同一発言者が出ない — 周回境界は許容しない: 前周の末尾と次周の先頭が同一なら swap)
- [ ] facilitator がターンループで発話しない (turn 0 のみ)
- [ ] round 2 以降の facilitator プロンプトに前ラウンド summary/winner が含まれる (プロンプト組み立ての単体テスト)
- [ ] 既存 `test:phase*` 通過
