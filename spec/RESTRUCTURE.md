# メカニクス分解・再構築パイプライン (RESTRUCTURE)

ゲーム (またはその一部コンテンツ) を **メカニクス単位に分解 → 評価 → 再構築** する分析
パイプラインの設計 spec。Discatier Core の KG (`Game` / `Mechanic` / `Aesthetic` /
`affect` / `playContext`) と SENTIMENT ベクトル、`GameFeedbackStore` を土台に、
「コアメカニクスに他ゲームの高評価メカニクスを結合して破綻を直す」 までを 1 本のフローに
通す。

> 本書は **設計と既知データでの試験計画** を定義する。実装は段階導入 (Phase 0 で粗く動かす)。
> 関連: [OVERVIEW](./OVERVIEW.md) / [core/DESIGN](./core/DESIGN.md) /
> [crawler/DESIGN](./crawler/DESIGN.md) / [crawler/SENTIMENT](./crawler/SENTIMENT.md) /
> [feature/game-feedback](./feature/game-feedback.md)

LUDIARS 短縮コード: **Di** / 主モジュール (予定): `src/restructure/`

---

## 0. 既存資産へのマッピング (新 schema を増やす前に)

このパイプラインは大部分が **既存 KG 語彙にそのまま乗る**。新規に要るのは「評価」
「ユーザ嗜好プロファイル」「内部経済」「core/option 区分」の 4 概念だけ。

| パイプラインの概念 | 既存資産 | 不足 (新規が要る) |
|---|---|---|
| ゲーム全体のメカニクス | `Game` -[HAS_MECHANIC]- `Mechanic` (KG md frontmatter) | — |
| メカニクスの狙う情緒 | `Mechanic.intends` → `Aesthetic` / `affect` | — |
| 文脈 (どの場面のメカニクスか) | `playContext` | — |
| ユーザ/集団の反応 | `GameFeedbackStore` (匿名) + SENTIMENT 20 次元ベクトル | — |
| 感情曲線 | SENTIMENT `sentiment_curve[]` (period × vector) | **進行軸の曲線** (後述、別物) |
| 論点・破綻 | `designGap` | — |
| 改善案・再構築案 | `hypothesis` (gap への解) | — |
| 類似メカニクス検索 | `vectors/` (`searchSimilar`, mechanic embedding) | — |
| **ジャンル/メカニクスへのユーザ感情** | (SENTIMENT は *ゲーム単位*) | **嗜好プロファイル** (genre/mechanic 粒度) |
| **メカニクス評価 (マクロ/ミクロ)** | — | **`MechanicEvaluation`** |
| **内部経済** (資源の流れ・収支) | — | **economy グラフ** (Mechanic 間の resource edge) |
| **コア/オプション区分** | — | `Mechanic.role` (core / option) |

新規ノード/属性は KG md の frontmatter 拡張で表現し (Phase 0)、DB schema 変更は後段に
寄せる (crawler/DESIGN と同じ「Phase 0 は既存 schema を使う」方針)。

---

## 1. パイプライン全体

```
 [蓄積]  ① ユーザ嗜好プロファイル        genre / mechanic → affect (好悪の事前分布)
            (game-feedback + SENTIMENT を粒度変換して継続蓄積)
              │
 [入力]  ② 分析対象を渡す
            ├─ 仕様書 (spec/md/Praeforma)  → メカニクスを *類推* (LLM 抽出)
            └─ 既知ゲーム / コンテンツ名    → KG から *取得*
              │
         ③ ゲーム全体のメカニクス集合を確定 (M = {mechanic_i})
              │
 [評価]  ④ マクロ / ミクロ評価
            ├─ マクロ: M 全体 + 内部経済 + 感情曲線 を評価
            └─ ミクロ: コンテンツ単体 (= 個々の機能) を評価
            (ゲーム全体レビュー時は各機能を個別にミクロ分析)
              │ → 評価を出力 (MechanicEvaluation[])
              │
 [分解]  ⑤ M を コアメカニクス Mc と オプション Mo に分解
              │
 [結合]  ⑥ Mc に、他ゲームの ④ で高評価なメカニクスを結合
              │   (嗜好プロファイル① と適合し、playContext が両立するものを選ぶ)
              │
 [修正]  ⑦ 結合で生じたメカニクスの破綻を検出・修正
              │
              ▼
        再構築されたメカニクス案 (= hypothesis 群 + 改訂 KG)
```

各ステップを designGap / hypothesis の語彙に落とすと、④ が `designGap` の発生源、
⑥⑦ が `hypothesis` の生成・検証に当たる。つまり本パイプラインは **既存の
gap→hypothesis ライフサイクルを「メカニクス再設計」目的で駆動する上位手順** である。

---

## 2. ステップ詳細

### ① ユーザ嗜好データの蓄積 (擬似集団)

**目的**: 「どのジャンル / どのメカニクスに対して どういう感情が持たれているか」 の
事前分布を、**擬似集団** (pseudo-collective) として継続的に貯める。

> **決定 (§4-A)**: 嗜好の主体は *個人ではなく擬似集団*。**YouTube コメント / Steam
> レビュー等の外部の声** (crawler/EXTERNAL-SOURCES + SENTIMENT) を母集団として集約した
> 「世間の感情分布」 を嗜好プロファイルとする。個人プロファイルは非ゴール (§5)。

- **母集団 (擬似集団の構成)**: 外部レビュー/コメントの集合。
  - steam (`appreviews`) / youtube (コメント) / reddit / website の外部発話
    (`crawl-channel` + `crawler/EXTERNAL-SOURCES`、SENTIMENT で 20 次元化済)。
  - `GameFeedbackStore` (Discord 匿名感想) も同列に混ぜる。
  - 個人を母集団の 1 サンプルとして数えるだけで、個人軸の保持はしない。
- **粒度**: 既存 SENTIMENT は *ゲーム単位* の 20 次元ベクトル。本ステップは
  **genre 単位 / mechanic 単位** に集約し直す (別軸)。
  - `pref(genre)` = そのジャンルのゲーム群の SENTIMENT を `volume_log` 加重平均。
  - `pref(mechanic)` = その mechanic を持つゲーム群の SENTIMENT を、mechanic が
    dominant aspect のクラスタに寄せて集約。
- **保存先**: `data/preferences/<scope>.json` (scope = `global` / `genre:<name>` /
  `mechanic:<name>`)。20 次元ベクトル + サンプル数 (`meta.volume_log`) + 出所内訳。
  active KG (`resolveActiveKgPath`) のタスク別 KG 単位で別々に貯められる。
- **個人データ**: 「出所は透明 / 個人は仮名」 (`feedback_data_accuracy_over_privacy` /
  EXTERNAL-SOURCES §6)。出所 (source + URL) は保持・開示するが、`authorId` /
  `authorName` は露出面でマスク。嗜好は匿名スコープ (`DISCATIER_WORKSPACE=knowledge`)。

### ② 分析対象の取り込み

渡されたものが **仕様書か実在ゲームか** で分岐する。

- **仕様書 (spec / md / Praeforma scene)**:
  KG に未登録の対象。本文から **メカニクスを類推 (LLM 抽出)** する。
  - 抽出は crawler/DESIGN の md frontmatter (`mechanics[]` / `aesthetics[]`) と
    同形式に正規化 → 一時 `Game` ノード (`source=spec`) として KG に投入。
  - `intends` (狙う情緒) は仕様書の記述から推定、無ければ空 (後で gap 化)。
- **既知ゲーム / コンテンツ名**:
  **学習済みデータから取得**。`Game` ノード + その `Mechanic` 群を KG から引く。
  無ければ ② の仕様書ルートにフォールバックするか、crawler に学習依頼を回す。
- 出力はどちらも **共通の中間表現** (Game + Mechanic[] + 既知 sentiment) に揃える。

### ③ ゲーム全体のメカニクス取得

② の中間表現から、評価対象の **メカニクス集合 M** を確定する。

- KG の `(Game)-[HAS_MECHANIC]->(Mechanic)` を全列挙。
- 各 Mechanic に `playContext` (戦闘 / 探索 / 育成 …) と `intends` (Aesthetic/affect)
  を紐付け。
- **内部経済はメカニクスから学習生成する** (決定 C): 内部経済は明示入力ではなく、
  **各 Mechanic の `description` / `intends` / `playContext` から LLM が資源の授受を
  推論して生成する**。
  - 生成物: `(Mechanic)-[PRODUCES|CONSUMES]->(Resource)` edge (新規)。例: 「敵撃破 →
    経験値 (produce)」「レベルアップ → 経験値 (consume)」「ショップ → 通貨 (consume)」。
  - `Resource` ノードは生成時に立ち上げ (時間 / 通貨 / 経験値 / 体力 / 素材 …)。
  - 生成は **決定論ではない** ので、Phase 0 はモック差し替え可能にし、生成済み
    economy グラフを `data/restructure/<slug>/economy.json` にキャッシュして再利用する。
  - これが ④ マクロ評価の「内部経済」入力になる。生成の不確実性は ④ の評価で
    収支不整合として顕在化するため、誤生成は下流で検出できる。

### ④ マクロ / ミクロ評価

評価は **2 スケール** で行い、両方を `MechanicEvaluation` として出力する。

- **ミクロ (コンテンツ単体)**: 個々の Mechanic / 機能を単体で評価。
  - 軸: 嗜好プロファイル① との適合度、SENTIMENT 由来の `asp.*` (fun/difficulty/
    content/…)、明快さ、習得コスト。
  - **ゲーム全体レビュー時は M の各機能をそれぞれミクロ分析** (= ミクロ評価の集合)。
- **マクロ (ゲーム全体)**: M 全体の構造を評価。
  - **内部経済**: ③ の resource グラフが閉じているか (生産=消費の収支、デフレ/
    インフレ、デッドエンド資源)。
  - **感情曲線**: 進行に沿った affect の起伏 (緊張↔解放のリズム、単調化の有無)。
  - **メカニクスの相互作用**: 相乗 (synergy) / 競合 (cannibalize) / 孤立 (isolated)。
- **出力**: 各評価を `score (0..1)` + `rationale` + 紐づく `affect` ベクトルで出し、
  ネガティブ評価は `bridge/gap/affect-negatives` 経由で `designGap` 化する。

> **決定 (§4-B)**: ここでの「感情曲線」 は *ゲーム進行軸* の曲線で、SENTIMENT の
> `sentiment_curve[]` (*外部レビューの時系列* = カレンダー軸) とは別物。混同しないよう
> `progression_curve` として区別する。実プレイデータは使わず、**仕様/メカニクス構造 +
> economy グラフからの推定でよい** (LLM が進行段階ごとに affect を起こす)。推定値である
> ことを `progression_curve.estimated=true` で明示する。

### ⑤ コア / オプション分解

M を **コアメカニクス Mc** と **オプション Mo** に分ける。

- **判定基準** (複数の重みづけ和、単一指標にしない):
  - 経済中心性: resource グラフでの次数中心性 (③) が高い = コア寄り。
  - ループ寄与: コアゲームループ (行動→報酬→強化→行動) に乗っているか。
  - 除去テスト: そのメカニクスを抜くとゲームが成立しなくなるか (LLM 反実仮想)。
  - 感情寄与: ④ マクロの感情曲線への寄与が支配的か。
- `Mechanic.role = core | option` を付与 (新規属性)。

### ⑥ 他ゲームの高評価メカニクスを結合

Mc に対し、**他ゲームの ④ で高評価だったメカニクス** を移植候補として結合する。

- **候補抽出**: KG 横断で `MechanicEvaluation.score` が高い Mechanic を引き、
  - 嗜好プロファイル① と適合 (`cosine(pref, mechanic.affect)` が高い)、
  - `playContext` が Mc と両立する、
  - `vectors/searchSimilar` で Mc と機能的に *補完* 関係 (近すぎ=重複は除外)
  のものを選ぶ。
- **結合**: 移植メカニクスを `hypothesis` (gap=「Mc を強化する」) として提案し、
  resource グラフへ仮接続する。
- **出所明示**: どのゲーム由来かを `hypothesis.provenance` に残す
  (`feedback_data_accuracy_over_privacy`: 出所は透明)。

> ⚠ レビュー論点 (§4-C): 「評価が高い」 ≠ 「Mc に合う」。高評価は *元ゲームの文脈* での
> 評価。移植は playContext / 内部経済の適合チェックを必須にする (盲目的な結合は ⑦ で破綻)。

### ⑦ メカニクスの破綻を修正

⑥ の結合で生じた **破綻 (incoherence)** を検出して直す。

- **破綻の定義** (検出ルール):
  - 内部経済の破綻: resource 収支が開く (無限インフレ / 枯渇デッドロック)。
  - 感情曲線の破綻: 起伏が消える (単調化) / 山が連続して飽和。
  - ループ未成立: コアループに戻らない一方通行のメカニクス。
  - 嗜好不一致: ① と逆符号の affect を強く誘発。
  - 重複/競合: 既存 Mc と役割が衝突 (cannibalize)。
- **修正**: 破綻ごとに `designGap` を立て、`hypothesis` で調整案 (パラメータ調整 /
  resource edge の再配線 / 採用見送り) を出す → ④ の評価に戻して再判定 (収束ループ)。
- 収束したら再構築案を確定出力 (改訂 KG md + hypothesis 群)。

---

## 3. 既知データでのテスト計画 (Phase 0)

実装前に **既に KG/SENTIMENT が揃っているゲーム** で各ステップの入出力を固定し、
ground truth 代わりの期待値を置く。

- **対象候補**: `data/games/` に存在する slug (例 `vampire-survivors`,
  `hollow-knight`)。少なくとも 1 件は SENTIMENT sidecar 付きを使う。
- **テストハーネス**: `scripts/restructure.ts <slug>` を CLI として用意し、
  各ステップの中間 JSON を `data/restructure/<slug>/step{1..7}.json` に吐く。
  Phase 0 は LLM 抽出をモック差し替え可能にして決定論テストにする。
- **ステップ別の確認**:

  | step | 入力 | 期待出力 | 既知データでの確認点 |
  |---|---|---|---|
  | ① | feedback + sentiment | `pref(genre/mechanic)` 20 次元 | サンプル数と符号が極端に振れない |
  | ② | slug or spec | 共通中間表現 | KG 取得経路と spec 類推経路が同形になる |
  | ③ | 中間表現 | M + resource グラフ | Mechanic 数が KG md と一致 |
  | ④ | M | `MechanicEvaluation[]` | ネガ評価が designGap になる |
  | ⑤ | 評価済 M | core/option ラベル | 既知のコアループ要素が core に入る |
  | ⑥ | Mc | 移植 hypothesis | provenance と playContext 適合が記録される |
  | ⑦ | 結合案 | 修正後 hypothesis | 経済収支/曲線の破綻が検出される |

- **回帰**: 既知ゲームの step③ (メカニクス集合) と step⑤ (core/option) は人手で
  正解を 1 度固定し、スナップショット比較する。④⑥⑦ は LLM 主観が入るので
  *構造的な不変条件* (収支が閉じる / provenance が必須 / gap↔hypothesis 対応) を
  assert する (数値一致は求めない)。

---

## 4. フローのレビュー (論点と決定)

依頼の「以上の流れをレビューする」 への回答。論点 A-G を洗い出し、A-F は方針決定済
(2026-06-09)。各論点に **[決定]** を併記する。

### A. ① の「ユーザの感情」 の主体 — **[決定] 擬似集団**

> Di の既存基盤 (`GameFeedbackStore` / SENTIMENT) は匿名・集団。フローの「ユーザの感情」
> も *個人ではなく擬似集団* と確定。

- **YouTube コメント / Steam レビュー等の外部の声** (crawler/EXTERNAL-SOURCES) を母集団
  として集約した「世間の感情分布」 を嗜好プロファイルとする (§2-①)。
- 個人プロファイル・同意境界は **非ゴール** (§5)。個人データ規約
  (`project_personal_data_rule`) との衝突を回避でき、既存 SENTIMENT 経路にそのまま乗る。

### B. 「感情曲線」 が 2 種類混在 — **[決定] 推定でよい**

- SENTIMENT の `sentiment_curve[]` (外部レビューのカレンダー時系列) と ④ マクロの
  **ゲーム進行軸の affect 起伏** (`progression_curve`) は別物。名前で明示分離する。
- 進行軸の曲線は **実プレイデータを使わず推定でよい** (確定)。メカニクス構造 + economy
  グラフから LLM が推定し、`progression_curve.estimated=true` を立てる。

### C. 「内部経済」 は KG に無い — **[決定] メカニクスから学習生成**

- resource の produce/consume edge は既存 schema に無い。④⑤⑦ の中心入力。
- **明示入力ではなく、各 Mechanic の description/intends/playContext から LLM が
  推論生成する** (確定、§2-③)。生成済みグラフはキャッシュし、誤生成は ④ の収支
  不整合で下流検出する。③ がパイプラインの肝なので優先実装。

### D. ⑥ の結合は「高評価」 だけでは破綻する — **[決定] AND 条件を採用**

評価は元ゲームの playContext での値。移植適合は (1) playContext 両立、
(2) 内部経済の収支、(3) 嗜好適合、の 3 条件を **AND** で課す。単に score 上位を
繋ぐと ⑦ の破綻が多発し収束しない。

### E. ⑦ の「破綻」 は自動検出が難しい — **[決定] 機械判定 + 議論回し採用**

経済収支・重複は機械判定できるが、「面白さの破綻」 は主観。検出ルール (§2-⑦) で
*機械判定可能なもの* に限定し、主観部分は LLM 評価 + designGap で人間/議論に回す
(Di の本領)。⑦→④ の収束ループに **反復上限** を置く (無限ループ防止)。

### F. ④⑥⑦ に LLM 主観が入る → ground truth が無い — **[決定] 構造 assert 採用**

既知データテスト (§3) は数値一致でなく **構造的不変条件** を検証する設計にした。
評価の再現性は seed/モデル固定 + 構造 assert で担保する。

### G. ステップは独立段階導入できる

①②③ (取り込み・KG 化・resource グラフ) は ④ 以降と独立に価値がある。
crawler/DESIGN の Phase 0→3 と同様、**「粗く動かす → 分類別リファクタ → 結合確認」**
(`feedback_prototyping_flow`) で段階導入するのが安全。

### 総括 / 推奨

- 既存 KG・SENTIMENT・gap/hypothesis に **8 割乗る**。新規は ①嗜好プロファイル /
  ③内部経済 / ④評価 / ⑤core-option の 4 点に集約できる。
- 実装順の推奨: **③内部経済 → ④評価 → ⑤分解 → ⑥結合 → ⑦修正**、①嗜好と
  ②仕様書類推は並行で先行プロトタイプ可。
- ⑥⑦ の収束ループは反復上限と構造 assert を必ず付ける。
- **論点 A-F は方針確定済 (2026-06-09)**: A=擬似集団 (外部レビュー母集団) /
  B=進行曲線は推定 / C=内部経済はメカニクスから学習生成 / D=AND 条件 /
  E=機械判定 + 議論回し / F=構造 assert。残る判断は実装時の細部のみ。

---

## 5. 非ゴール (Phase 0 ではやらない)

- DB schema 変更 (resource edge / role / evaluation は Phase 0 は md frontmatter +
  sidecar JSON で表現)。
- 実プレイテレメトリ由来の感情曲線 (推定に留める)。
- 個人嗜好プロファイル (集団のみ)。
- フロントエンド可視化 (評価・再構築案の UI は後段)。
- Discord slash 統合 (`/restructure <game>` は Phase 2 候補)。
