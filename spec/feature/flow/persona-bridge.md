# Voluptas ペルソナブリッジ — 設計

> 状態: ドラフト (2026-07-27)。2026-07-27 ペルソナエンジンレビュー (Vo/Di 横断) の Di 側対応。
> 対リポ側設計: Voluptas `persona-engine-v2-design.md` (エクスポート形式 v2・同意・母集団レポート取込)。
> 関連: [persona-pool.md](./persona-pool.md) (プール/憑依/採用/合成)、
> `spec/feature/crawler/SENTIMENT.md` (20 次元空間の正本)。

## 0. 背景 (レビュー所見)

- Voluptas は仮名ペルソナ (pseudoId + 20 次元 affect + traits) のエクスポートスクリプトを
  持つが、Di 側に取込先が無く、両リポで揃えた 15 軸語彙・20 次元空間が未接続 (R4)。
- Di の採用ペルソナ (`論者#xxxx`) は affect ベクトルと極性特徴のみで属性が無く、
  憑依 descriptor が薄い。合成ペルソナの属性 (年代・課金) は LLM の想像 (P2)。
- 20 次元空間の実装が二重化している: Di は `src/flow/sentiment-vector.ts` (crawler lexicon 移植)、
  Vo は `@ludiars/sentiment-core` (Lapilli)。仕様同一のまま実装が乖離するリスクがある。

## 1. インポート (Vo → Di)

### 1.1 入力

Voluptas エクスポート v2 (JSONL、詳細は Vo 側設計 §6.1):
`pseudoId / affectVector(20)+vectorSpecVersion / preferenceAxes(15軸) / aversions / traits /
attributes{ageBand?,spending?} / mechanicReactions / exportSpecVersion:2`。
同意済みユーザのみ・仮名のみが前提 (Vo 側で担保)。

### 1.2 取込

- CLI: `npm run persona:import -- --file <jsonl> | --url <voluptas export endpoint>`
  (`src/flow/persona-import.ts` + `scripts/persona-import.ts`)。
- `flow_persona` へ upsert:
  - `origin = "imported"` (新値。既存: adopted / generated / synthesized)
  - `source_speaker_id = "vo:<pseudoId>"` (再取込で別個体を量産しない。adopted の
    `ext:<source>:<authorId>` と同じ規約)
  - `affect_vector` = affectVector (vectorSpecVersion 一致を検証、非一致は skip + 警告)
  - 露出名は `論者#xxxxxx` (`maskedPersonaLabel` 流用。個人データ方針は adopted と同一)
- 新カラム (migration `flow_00xx_persona_import`):
  - `preference_axes_json` — 15 軸スコア (adopted/generated にも将来書ける汎用列)
  - `attributes_json` — ageBand / spending 等の任意属性
  - `aversions_json` — 忌避
  - `mechanic_reactions_json` — メカニクス反応
- `--url` は Cernere project credential での認証 (Vo 側 §6.2)。認証情報は config
  (`voluptas.{baseUrl, clientId, clientSecret}`) に置き、ログへ出さない。

### 1.3 検証

- exportSpecVersion / vectorSpecVersion / 次元数 20 / スコア域 (0..1) を取込時に検証し、
  不正行は skip + 件数レポート (取込全体は止めない)。
- dry-run (`--dry`) で採用予定件数と skip 理由内訳を出す (persona:adopt と同型)。

## 2. 憑依 descriptor の強化

`toFlowPersona` / 憑依 (`selectPossessionByTheme`) が imported ペルソナを選んだとき、
`PersonaPossession.descriptor` を構造化データから決定的に組み立てる (LLM 不使用):

```
preference_axes_json の上位 2 軸 + 下位 1 軸 (「探索と物語を強く好み、日課要素は好まない」)
+ aversions (「ガチャの天井なしを嫌う」)
+ attributes (「20代 / 微課金」)
+ mechanic_reactions 上位 (「回避アクションに強く反応」)
```

- テンプレートは `src/flow/persona-descriptor.ts` の純関数 (テスト対象)。
  axis id→日本語表現の辞書は `persona-questionnaire.ts` の `PREFERENCE_AXES` label を流用。
- adopted ペルソナも同関数を通す (使える材料が affect/極性特徴だけなら従来相当の薄い
  descriptor に degrade)。既存挙動を壊さない。
- 憑依の目標ベクトル選定 (テーマ類推 cosine) は従来通り affect 空間。preference_axes は
  descriptor と将来のフィルタ (「課金に敏感な論者を混ぜる」等) に使い、選定関数は変えない
  (変える場合は別 spec)。

## 3. 母集団レポート (Di → Vo)

- `persona-populations.ts` の `estimatePopulations` を拡張し、imported ペルソナも
  クラスタリング対象に含める (`realOrigins` = adopted は従来通り実分布側)。
- 新 CLI 出力: `npm run persona:populations -- --report <path>` で
  `population-report.json` を書き出す:

```jsonc
{ "generatedAt": "…", "realPopulation": 1836,
  "entries": [ { "pseudoId": "…",            // vo: プレフィクスを剥がした Vo 側キー
                 "verdict": "major" | "minor",
                 "ratio": 0.083,              // 実分布近傍比率
                 "nearestClusterSize": 152 } ] }
```

- Vo が取り込んで persona.json v2 の `population` に書き戻す (Vo 側 §6.3)。
- 実行は当面手動バッチ (crawl 後の persona:adopt と同じ運用)。自動化は
  `flow.autoAdoptOnCrawl` と同型の opt-in フラグを将来足す。

## 4. 議論ログの還流 (Di → Vo) — 後段フェーズ

本人が Di の議論で発した発言を、本人の Voluptas evidence に返す。

- **同意とアカウント紐付けが前提**: Vo 側で「Discord アカウント <id> を自分として紐付け、
  発言の還流に同意」した場合のみ。紐付け情報は Vo (Cernere) 側が持ち、Di は
  「このユーザ id の発言をエクスポートして」という **pull 要求に応えるだけ** にする
  (Di に Vo のユーザ対応表を持たせない。匿名 workspace 方針を保つ)。
- エクスポート: `GET /api/persona-bridge/utterances?authorId=<discord id>&since=` (admin/
  project credential 認証)。返すのは human 発話 (`flow_utterance` の human 分 + web-chat) の
  本文とタイムスタンプのみ。persona (AI) 発話・他人の発話は返さない。
- Vo 側は voice 相当の evidence として取り込む (Vo 側 §4.6)。

## 5. sentiment 空間の一本化 (大部分は main で実施済み — 2026-07-27 訂正)

- **実施済み (main)**: `@ludiars/sentiment-core` の `file:` 依存追加、
  `src/flow/sentiment-vector.ts` の sentiment-core re-export 化 (flow 層 + `cascade.ts`)。
  初版スペックはレビュー時に旧ブランチのチェックアウトを参照していたため「未着手」と
  誤記していた。
- **残り**: `src/crawler/sentiment/analyze.mjs` (crawl バッチ最適化 n>1 集計) が独自
  lexicon.json を持ったまま。
  1. `VECTOR_SPEC_VERSION` と次元名配列の sentiment-core との一致を **起動時 assert** する。
  2. lexicon.json の変更は今後 sentiment-core 側で行い、Di の crawler lexicon は
     そこから同期する (乖離したら assert が落ちる)。
- ブリッジの取込検証 (§1.3) が vectorSpecVersion を見るため、assert 未実装でも取込は安全に
  動く (バージョン不一致なら skip されるだけ)。一本化残作業はブリッジの前提ではなく並行タスク。

## 6. 非目標

- 憑依の選定アルゴリズム変更 (affect cosine のまま)。
- embedding ベース affect (#125 別トラック)。
- Vo 側スキーマ・UI (Vo 設計の範囲)。
- imported ペルソナの Discord 表示名に Vo 由来であることを出す (出さない。論者# で統一)。

## 7. 実装タスク分解 (フルセット)

| # | タスク | 依存 |
|---|---|---|
| D1 | migration + `persona-import.ts` (+ CLI, dry-run, 検証, upsert) + テスト | Vo T13 (形式確定) と形式合意のみ。ファイル取込はスタブ JSONL でテスト可 |
| D2 | `persona-descriptor.ts` (構造化 descriptor 純関数) + 憑依/壁打ち/toFlowPersona 配線 + テスト | D1 |
| D3 | `estimatePopulations` の imported 対応 + `--report` 出力 + テスト | D1 |
| D4 | utterance エクスポート API (認証 + human 発話 filter) + テスト | なし (Vo T16 と同時リリース) |
| D5 | sentiment-core 一本化の残作業 (analyze.mjs の起動時 assert + lexicon 同期方針) + テスト | なし (並行可。dep追加とre-exportはmainで実施済み) |
