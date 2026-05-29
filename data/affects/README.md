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

## 実装済み (この PR)

- **seed ローダ**: `npm run seed:affects` … `vocabulary.json` → `affects` テーブル(workspace=`knowledge`、`id=affect:<key>` で upsert、`vocabulary_status`=`status`(既定 canonical)、`valence` セット)。
- **matcher 負判定の語彙化**: `src/core/bridge/gap/affect-negatives.ts`(`valence==="negative"` から生成)を matcher が参照。`npm run seed:affects -- --emit-negatives` で再生成。`monotony`/`confusion`/`unfair_monetization` も負ギャップに効く。
- **intended_affect マッピング**: `data/games/*.md` の `mechanics[].intended_affect` はこの `key` を使う(sample-hollow-knight に適用済)。

## 未知語フロー (単語登録質問 → LLM 調査 → provisional → 修正可能)

`src/crawler/sentiment/affect-resolver.mjs` の `resolveAffect(term)`:

1. **既知** (`key` or `label_ja` 一致) → `{status:"known", key}` を返す。
2. **未知** → **計算せず** `data/affects/pending-questions.json` に質問を積む(`{status:"queued"}`)。

`npm run investigate:affects`(`ANTHROPIC_API_KEY` 必要 / 無ければ件数表示の no-op):

3. queued を **LLM に調査**させ(`{key,label_ja,valence,description,alias_of}`)、`vocabulary.json` に
   `status:"provisional"`, `provenance:"llm"` で追加。既存 key に寄せられる場合は alias 扱い。
4. **provisional 語は後で人手修正可能** — `vocabulary.json` を編集(valence/label/統合)→ `npm run seed:affects` で反映。
   人手確定時は `status` を `canonical` に上げる。

> 設計対応: Discatier の「未語彙化感情の保護」(既存 Affect にマップされない発話を語彙議論へ回す)に相当。
> ベクトル化は語彙確定後に行うため、未知語のまま誤った次元へ押し込まない。
