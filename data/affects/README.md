# Affect 統制語彙 (サンプル / Phase 0)

`vocabulary.json` は議論で使う **Affect(ゲーム体験の感情・美的体験)の統制語彙**。
Axis1 の「意図された体験」と Axis2 の「観測された体験」を**同じ `key` の完全一致**で突き合わせるための辞書。

## なぜ必要か (matcher の仕様)

`src/core/bridge/gap/matcher.ts` の Gap 判定は:
- `mechanics.intended_affect` と 観測 affect(`translation_proposals.target_name` / `affects.mood`)を **完全文字列一致**で比較
- 強い負感情は **ハードコード集合 `["frustration","anger","sadness","boredom"]`** で判定

→ 両者が同じラベルを使わないと Gap が立たない。本語彙はその共通辞書で、
負語のうち `frustration / anger / sadness / boredom` は matcher の綴りに合わせてある。

## スキーマ (1 エントリ)

| field | 説明 |
|---|---|
| `key` | 完全一致用の正規ラベル(英 snake)。`intended_affect` / 観測 affect は必ずこの key を使う |
| `label_ja` | 表示用の日本語名 |
| `valence` | `positive` / `negative` / `ambivalent` |
| `mda` | (任意) MDA「8 つの楽しさ」との対応 |
| `description` | 短い定義 |

## 24 語の内訳

- positive 14 / negative 7 / ambivalent 3
- ambivalent(`fear` `suspense` `solitude`)は文脈で正負反転(ホラーの恐怖は意図された正、等)

## 使い方 (運用)

- crawler / 手書き md の `mechanics[].intended_affect` はこの `key` を使う。
- sentiment crawler の観測 affect も Translation Bridge でこの `key` へ写像する
  (汎用感情 Plutchik → ゲーム Affect 語彙)。

## TODO (follow-up)

- **seed ローダ**: `vocabulary.json` → `affects` テーブル(workspace=`knowledge`)へ投入する script。
- **matcher 小改修(任意)**: 負判定を「`valence == "negative"` を語彙から引く」方式にして、
  `monotony` / `confusion` / `unfair_monetization` も負ギャップに効かせる。
