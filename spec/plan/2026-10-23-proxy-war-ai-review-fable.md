# 代理戦争 AI — 実現可能性・実効性レビュー (Fable 案)

> 状態: 設計レビュー (2026-10-23)。依頼: `spec/tasks/2026-10-23-proxy-war-ai-review-request.md` (Fable / Astra 二本立て、親 Opus が統合)。
> **追加指示 2 (2026-10-23) を最優先で反映済み**: ペルソナ形成用データマイナー = **Histrio** (`E:/Document/Ars/Histrio`、
> spec のみでコード無し)。追加指示 1 の論点 10・11 は「Histrio が持つべき形成の範囲と Di への出力契約」「Histrio ↔ Di の境界と
> `/v1/render` の使い分け」として書き、責務境界は Voluptas ↔ Histrio ↔ Di の三者で書いた。
> 本書は実装しない。もう一方の案 (Astra) は参照していない。
> 根拠に使った正本: Histrio `README.md` / `spec/feature/persona-chat-engine.md` / `spec/feature/role-model-engine.md`、
> Di `spec/feature/flow/persona-pool.md` (canonical)、`persona-bridge.md`、`dialectic.md`、`improvement.md`、
> `gemma-ft.md` §9-4、`voice-rag-hybrid.md`、`spec/feature/crawler/SENTIMENT.md`、
> `src/flow/persona-{adopt,adopt-runner,pool,descriptor,cast,turn,setup}.ts`、`src/flow/design-gap.ts`、
> `src/flow/dialectic/exchange-prompt.ts`、`src/crawler/steam-persona/collect.ts`、`src/crawler/sources/persona.ts`。

---

## 0. 結論

### 総合判定: **条件付き成立**

「FT より、ベクトル化データを食わせる方が高速で精度が高い」は、**「ベクトル」を『20 次元の数値』ではなく
『数値から機械的に言語化した嗜好要約 + ジャンル別 design_gap + 本人の代表発言 (原文抜粋・出所付き)』の束**と読み替えた
ときに成立する。数値ベクトルをそのまま prompt に置く方式は不成立 (§3)。
この束を作って正本として持つのが **Histrio**、受け取って代理同士を議論させるのが **Di** という分担 (§9〜§11) で、
Di の既存資産 (`steam-persona/` の収集、`persona-adopt.ts` の集約、`design-gap.ts` の差分、`voice-search.ts` の rerank) は
Histrio の形成層の前身として切り出せる。ただし Histrio の現 spec (seed + signals の heuristic、LLM 不使用) には
**実在話者の公開発言から人物像を作る機能が無い**ので、それを「証拠ペルソナ (evidence persona)」として足す必要がある (§10)。

| 軸 | 判定 | 一言 |
|---|---|---|
| (a) 生成コスト・速度 | **成立** | FT は人物ごと/更新ごとに学習が要る。in-context は Histrio の offline 集計 + Di の prompt 構築のみ |
| (b) 人物の再現精度 | **条件付き成立** | 20D affect は「何を好む / 嫌う」は持つが「何を主張するか」を持たない。代表発言 (RAG) を束ねて初めて FT 同等以上 |
| (c) 議論の質 | **条件付き成立** | 立場の一貫性・捏造防止は構造 (dialectic + 根拠縛り) で担保する。FT はここに寄与しない |
| (d) 更新容易性 | **成立** | Histrio が再集計して契約の新版を出せば Di は取り込み直すだけ。FT は再学習 |

### 一番の懸念 3 つ

1. **同一化 (identity collapse)**: 20D affect はレキシコン由来の感情プロファイル (valence / arousal / Plutchik 8 / aspect 8 / meta 2)
   であり、同じジャンルを好む 2 人のベクトルは近い。数値や短い descriptor だけで 2 人を演じ分けさせると、
   数ターンで両者が「同じ意見の別名」に寄る。**本人発言の原文を根拠に縛る**ことが唯一の歯止めで、
   Histrio の出力契約に**発言原文を含めない構成は実効性が無い** (§3・§7・§10)。
2. **言っていない主張の帰属**: 代理は本人が言っていないことを必ず言う (議題が本人の発言範囲を超えるため)。
   「本人発言に由来する部分」と「代理が推論した部分」を**データとして分離し露出面でも区別**しなければ、
   仮名化していても「あの論者#xxxx がこう言った」という誤帰属が残る (§7・§8)。
3. **ジャンル別 design_gap を作れる人が少ない**: 既存 C1 条件 (全ゲーム横断 10 件以上) は affect 1 本を作る条件であって、
   **ジャンル単位**で「目標 − 現状」を作るには同ジャンル内に正負両方の発言が要る。実データで何人が条件を満たすかは
   未計測で、ここが薄いと「代理」ではなく「ジャンルの平均人」になる。契約に**充足度 (件数・正負・信頼度)**
   を必ず載せ、Di 側で薄い profile を弾く (§4・§10)。

### 推奨方式 (要約)

**in-context ハイブリッド** = (A) 言語化 descriptor (20D + 追加特徴から決定的に生成) + (B) ジャンル別 design_gap の言語化
(望む状態 / 不満の状態 の上位次元) + (C) 本人の代表発言 原文抜粋 (出所付き・ID 付き) + (D) 「立場カード」(主張 ↔ 根拠発言 ID の対応表)。
(A)〜(D) は **Histrio が作って `GET /v1/personas` (namespace `di`) の契約 (§10) で渡す**。Di は **1 人 1 ジャンル ≤ 3.5k トークン** (§5)
にクランプし、既存 dialectic エンジンに「憑依 (possession)」の拡張として載せる (§7)。**論証の生成は Di の既存 persona-turn /
exchange-prompt、声色の整形だけを Histrio `/v1/render` に任意で委ねる** (§11)。FT は採らない (§2)。
評価は契約に同梱する held-out の正解ラベルで **推薦予測**と **判別テスト**を Di 側で数値化する (§6)。

---

## 1. 解釈 (親の解釈への同意・異論)

| # | 親の解釈 | 同意 / 異論 |
|---|---|---|
| 1 | 代理戦争 AI = 実在人物の立場・嗜好を再現して代理同士を議論させる | **同意**。ただし「実在人物」の範囲を **公開発言の仮名話者に限定**することを前提に置く (§8)。Discord ユーザや実名の開発者・配信者を対象にする設計は本書では扱わない |
| 2 | Vo 関係なく = Di の材料だけで形成 | **追加指示 1・2 で訂正済み**: 人物像の形成と正本は **Histrio**。Di は「受け取った人物像で代理同士を議論させる側」(§9〜§11)。既存 `steam-persona/` (出力コメントに「Vo / Histrio が読む JSONL」と明記あり) と `persona-adopt.ts` は Histrio の形成層の前身として §10 で評価した |
| 3 | ペルソナデータ = ジャンル単位 design_gap + 抽象ベクトル → prompt で in-context 条件付け | **半分同意**。design_gap の概念流用は妥当。ただし design_gap と 20D だけでは人物を特定できず、**本人発言の原文**が必須 (§3)。「ベクトルを食わせる」の中身を再定義する |
| 4 | 10k = 1 ジャンル 1 人の prompt 量 ≈ 10k トークン | **異論あり**。10k トークン/人は同時に 2〜4 人載せると既存ペーパー + 外部の声と合算で 40k 超になり、注意が薄まり同一化を招く。**人 1 ジャンル ≤ 3.5k トークン、1 ターンに載せる代理データ ≤ 7k** と読み替える。別途、Histrio 側の「1 ジャンルの材料 ≤ 10k 件」も上限として意味がある (§5) |
| 5 | 主題 = 仮説を検証可能にする | **同意**。§2 で 4 軸に分解、§6 で測り方を定義 |

**親の解釈に無い指摘**: 既存の **「立場固定 (pro/con をセッション開始時に割る)」は代理と相性が悪い**。
代理は議題に対して「本人ならどちらか」を**データから導く**べきで、index で pro/con を割る `persona-cast.ts` の
`specifiedStance` をそのまま使うと、本人と逆の陣営で戦わされる (§7)。

---

## 2. §4-1 仮説の分解 — FT / in-context / RAG / 併用の比較

### 比較表

| 観点 | FT (QLoRA, 人物別 adapter) | in-context: 数値 20D + design_gap のみ | in-context: 言語化 + design_gap + 代表発言 (RAG) | 併用 (口調 FT + in-context 立場) |
|---|---|---|---|---|
| (a) 生成コスト・速度 | 人物 × 更新ごとに学習 (`gemma-ft.md` 規模で 5k〜20k サンプル/モデル)。人物別は現実的でない (サンプル不足) | Histrio の offline 集計のみ。prompt は ~500 トークン | offline 集計 + 1 回の抽出 (立場カード)。prompt 2.5k〜3.5k/人 | FT の負担がそのまま残る |
| (b) 人物再現 | 口調は出る。立場は学習サンプルが本人発言 10〜100 件では乗らない (過学習か無視) | **不成立**。LLM は 20 個の 0..1 を「人物」に写像できない (§3) | 立場は原文に縛られるので最も高い。口調は発言抜粋から模倣 | 口調 + 立場で最高だが FT 分のコスト |
| (c) 議論の質 | 本人以外の議題で捏造しやすい。根拠の出所を追えない | 一般論に寄る (同一化) | 根拠 = 発言 ID で追跡可。dialectic のトゥールミン検査と相性が良い | 同左 |
| (d) 更新容易性 | 再学習 | 再集計 | 再集計 + 立場カード差分抽出 | 再学習が残る |
| 既存資産の流用 | `gemma-ft.md` パイプライン (議論全体の蒸留用で人物別ではない) | `persona-adopt.ts` / `design-gap.ts` | 上に加え `voice-search.ts` (rerank + MMR)、`persona-descriptor.ts` | — |

### 判定

- **(a) 成立**。FT は「人物ごとの adapter」を前提にすると学習データが決定的に不足する (1 人の公開発言は数十件。
  `gemma-ft.md` §2-2 の 5k〜20k サンプルに 2 桁足りない)。全人物混合 FT なら作れるが、それは「人物の再現」ではなく
  「レビュアー口調の再現」で、仮説の対象ではない。
- **(b) 条件付き成立**。条件 = 代表発言の原文を必ず同梱する。人物の同一性は「どのメカニクスをどう評価したか」
  という**命題の集合**にあり、20D affect はその射影 (感情・aspect の活性度) でしかない。射影から命題は復元できない。
- **(c) 条件付き成立**。質は prompt の中身ではなく **構造** (立場固定・根拠縛り・捏造の分離) で決まる。
  FT はこの構造に寄与しない。in-context はこの構造を「発言 ID を根拠に書け」という形で素直に組み込める。
- **(d) 成立**。

### 「FT の方が良い」が成り立つ条件 (反証条件、記録)

- 1 人あたり発言が 1,000 件超あり、かつ口調 (一人称・文末・絵文字) が本人性の主要素になる場合 (配信者・著名レビュアー)。
  本件の対象 (公開レビュアー、1 人 10〜100 件) には当てはまらない。
- ローカル LLM (Gemma 12B 級) で代理を動かし、長い prompt の追従力が足りない場合。この場合も「口調 FT + 短い in-context」
  の併用が先で、人物別 FT ではない。

---

## 3. §4-2 ベクトルを LLM に「食わせる」意味

### 判定: 数値のまま = **不成立** / 言語化 = **条件付き成立 (補助)** / 代表発言抜粋 = **成立 (主役)**

| 形 | LLM が人物を演じ分けられるか | 根拠 |
|---|---|---|
| 数値のまま (`[0.62, 0.41, …]` 20 個) | ✕ | 次元名が無ければ意味不明。次元名を付けても「fear 0.31」から発言内容は出ない。2 人の差が 0.05 なら LLM は同じ扱いにする |
| 言語化 descriptor (既存 `buildPersonaDescriptor` の延長: 上位/下位次元・極性偏り・ばらつき) | △ | 「難易度と達成感を重視し、課金に厳しい、好き嫌いがはっきりしている」は**役割指示**として効くが、同系統の人は同じ文になる。識別子ではなく**傾向の枠** |
| design_gap の言語化 (ジャンル内で「望む状態 − 不満の状態」の上位 3 次元ずつ) | ○ | 人物の**向き**を与える。「このジャンルでは story と content に不満、replayability と difficulty を望む」は主張の種になる |
| 代表発言の原文抜粋 (出所 + 発言 ID) | ◎ | 命題そのもの。模倣も根拠縛りもここから。**これが無いと他の 3 つは意味を持たない** |
| 立場カード (主張 1 文 ↔ 根拠発言 ID) | ◎ | 議論中の「本人ならこう言う」を**毎ターン推論させず**固定する。抽出は人物 × ジャンルで 1 回 (抽出方法は §10) |

### 埋め込み (bge-m3 等) の扱い

埋め込みは **LLM には渡さない**。用途は 2 つに限定する:

1. **代表発言の選抜** (Histrio 側): 人物の発言群から「ジャンル内で代表的 + 多様 (MMR)」な k 件を選ぶ。
   Di の `voice-search.ts` (RRF + MMR) と同じアルゴリズムを Histrio に持たせる。議題ごとの再選抜は Di が契約の
   `evidence[]` 内で行う (§5)。
2. **評価** (§6、Di 側): 代理の発話と本人の held-out 発言の近さ、代理同士の近さ (同一化の検出)。

20D affect の用途は従来どおり **選定 (近傍) と descriptor の材料**。prompt には数値を出さない。

---

## 4. §4-3 ペルソナデータの生成手順 (Histrio の形成層への提案)

### 判定: **条件付き成立** (既存 C1 条件では足りない。ジャンル単位の追加条件が要る)

生成は **Histrio の責務** (§9・§10)。ここでは Di の既存資産から流用できる手順として書く。

```
[1] 話者集約      公開発言を speaker (仮名アンカー) × game で集約 (Di の adopt-runner と同じ入口)
[2] ジャンル付与  game → genre[] (下記)。1 ゲームが複数ジャンルに属してよい
[3] 人物 × ジャンル 単位で:
     a. 発言数・極性分布 (votedUp / valence) を数える → 充足判定 (下記)
     b. 望む状態 W = 正の発言 (votedUp=true または valence>0.5+eps) を連結 → textToVector
        不満の状態 N = 負の発言 を連結 → textToVector
        person_gap = W − N  (20D。aspect 次元が主役、emo/meta は参考)
     c. 代表発言 = 埋め込み rerank + MMR で k 件 (正負を半々に近づける)。発言 ID と出所を保持
     d. 立場カード = 代表発言から「このジャンルでの主張」3〜6 件を {claim, polarity, evidenceIds[]} で作る。
        evidenceIds に無い主張は捨てる (捏造防止はここから)。決定的版 / LLM 補助版は §10
     e. held-out = 評価用に一部のゲームを伏せる (契約に正解ラベルだけ載せる)
[4] 出力          出力契約 (§10)
```

### 「現状」「目標」を人物の何から取るか

| 案 | 現状 | 目標 | 評価 |
|---|---|---|---|
| A. 本人発言の正負分離 (推奨・Histrio 側) | 本人が不満を書いた発言の aspect 活性 | 本人が褒めた発言の aspect 活性 | 本人の内側だけで閉じる。発言が正負に揃わないと作れない |
| B. 議題ゲーム vs 本人の好きなゲーム (Di 側、議論ごと) | 議題ゲームの `game_vector` (`loadCurrentVector`) | 契約の `likedGames[]` の `game_vector` 平均 | 議題固有の gap。本人が議題ゲームを遊んでいなくても作れる。`data/games` 依存 |
| C. 既存 improvement の流用 | 同左 | メカニクス `intended_affect` | 人物に依らない = 代理の意味が無い。不採用 |

**推奨: A を正本 (Histrio)、B を議題ごとの補助 (Di)**。A が作れない人は B だけで degrade し `method` を記録する。
`computeDesignGap` は `subtract(target, current)` の純関数なので、どちらも同じ関数で計算できる (sentiment-core を両者で共有)。

### 必要な最小発言数 (提案値、§6 の評価で調整)

| 単位 | 既存 C1 | 提案 |
|---|---|---|
| 全ゲーム横断 | ≥ 10 件、正負両方、≥ 2 ゲーム | 維持 (人物をプールに入れる条件) |
| 人物 × ジャンル (profile 作成条件) | — | **≥ 8 件、うち正・負それぞれ ≥ 2 件、≥ 2 ゲーム** |
| 代表発言 k | — | 6〜10 件 (予算 §5 から逆算) |
| 立場カード | — | 3〜6 件。evidenceIds が空のカードは出さない |

根拠: 公開レビューは 1 件 100〜800 字で、8 件あれば aspect 次元の活性が中立 0.5 から離れる (`persona-adopt.ts` が
「ゲーム単位で連結してから 1 回ベクトル化」にした理由と同じ)。正負各 2 件は gap の符号を安定させる最小。
**実データでこの条件を満たす人物数は未計測**。Histrio 側の受入に「条件を満たす人物 × ジャンル数のレポート」を含める。

### ジャンルの切り方

| 源泉 | 長所 | 短所 |
|---|---|---|
| Steam storefront `appdetails.genres` (Vo v2 §3.2.1 と同じ) | 公式・取得容易 | 粗い。steam 以外の source に無い |
| Ludus game-lexicon | LUDIARS 内で一貫 | ジャンル表ではなくメカニクス表 |
| `data/games/*.md` frontmatter | Di に既存 | 件数が少ない |

**推奨: ジャンル表は Histrio の静的辞書 (Steam genres 一次)。契約の `genre` は自由文字列ではなく辞書の id** とし、
Di は id → 日本語ラベルの対応だけ持つ。LLM で推定しない (Histrio の heuristic 原則と一致)。

### 既存 C1 採用条件で足りるか

**足りない**。C1 は「プールに入れてよい人か」の条件で、「このジャンルで代理にできるか」は別の条件 (上表)。
極性偏り除外 (全ポジ/全ネガ) は**ジャンル単位では外す**: ジャンル内で全ポジの人は「このジャンルの擁護者」として
代理の意味がある (gap は B 案で作る)。

---

## 5. §4-4 10k 予算

### 判定: **条件付き成立** (1 人 10k は不可、1 人 ≤ 3.5k・1 ターン ≤ 7k なら成立)

### 1 人 × 1 ジャンルの内訳 (トークン、日本語 ≈ 1.3 トークン/字で概算)

| 区画 | 内容 | 目安 | 上限 |
|---|---|---|---|
| 言語化 descriptor | 上位 2 / 下位 1 次元、極性偏り、ばらつき | 120 | 200 |
| ジャンル別 design_gap の言語化 | 望む上位 3 次元 / 不満上位 3 次元 + 一言 | 150 | 250 |
| 立場カード | 3〜6 件 × (主張 1 文 + evidenceIds) | 400 | 700 |
| 代表発言 原文抜粋 | 6〜10 件 × 各 ≤ 200 字 (超過は文の区切りで切る) + 出所 + ID | 1,800 | 2,200 |
| 口調メモ | 一人称 / 文末 / 句読点の癖を 2〜3 行 (頻度集計で決定的に) | 80 | 150 |
| **合計** | | **≈ 2,550** | **3,500** |

### 複数人同時 + 既存ペーパー・外部の声との合算

| 構成 | 代理 (1 ターン) | ペーパー base (system、キャッシュ) | 外部の声 (`paperRichness.voices`=15) | 議論状態ダイジェスト | 合計 (user 側) |
|---|---|---|---|---|---|
| 代理 2 人 (1 対 1) | 3.5k (発話者) + 相手の立場カード 0.7k | 4〜8k (固定) | 2.3k | ~1.5k | ≈ 8k + system |
| 代理 4 人 | 同じ (発話者 1 人分のみ) | 同 | 同 | 同 | ≈ 8k + system |

**各ターンの prompt に入れるのは「発話する代理 1 人分の束」+「応答相手の立場カード」だけ**で、相手の代表発言は入れない
(相手の主張は dialectic の Position ダイジェストに既に入っている)。代理人数が増えても 1 call の prompt は一定。

### 圧縮を Histrio 側でやるか Di 側でやるか

| 処理 | 置く側 | 理由 |
|---|---|---|
| 発言の選抜 (k ≤ 12)・立場カード・descriptor/gap の材料 (数値 + 上位次元 id)・口調メモ | **Histrio** | 材料 (全発言・埋め込み・生 ID) を持つのは Histrio。Di に全発言を送らない方が個人データ上も軽い |
| 日本語への言語化・トークン予算へのクランプ (3.5k)・文区切り切断・議題に応じた evidence の並べ替え | **Di** | 予算はモデルと議論構成に依存する Di の都合。既存 `persona-descriptor.ts` が Di にある |

### 他の「10k」の読み

| 読み | 妥当性 |
|---|---|
| 1 ジャンルの材料 (発言件数) ≤ 10k 件 | 妥当。Histrio 側の集計上限として置く (埋め込み構築が線形) |
| 1 ジャンルのプロファイル全体 ≤ 10 KB | 不適。代表発言込みで 1 人 6〜10 KB になり、1 ジャンル複数人で超える |

---

## 6. §4-5 議論精度の測り方

### 判定: **成立** (契約に held-out の正解ラベルを同梱すれば客観評価できる。既存 eval の枠組みを流用)

### 指標

| # | 指標 | 測り方 | 合格基準 (案) |
|---|---|---|---|
| M1 | **held-out 推薦予測一致率** | 契約の `heldOut[]` (伏せたゲーム + votedUp 相当のラベル) について代理に「あなたはこのゲームを勧めるか」を Yes/No で答えさせ比較 | 正解率 ≥ 0.70 かつ **多数派ベースライン + 0.15 以上** (全ポジ型は多数派だけで 0.9 出るため差分で見る) |
| M2 | **立場予測一致率 (aspect)** | held-out の aspect 別極性 (難易度を褒めた/けなした) と代理の回答を比較 | macro-F1 ≥ 0.60 |
| M3 | **判別テスト (blind)** | 判定 LLM に「本人の held-out 発言 3 件」と「代理 A / 代理 B の発話」を渡し、どちらが本人か当てさせる。`gemma-ft.md` §9-4 の blind 勝率の枠組み (judge prompt を spec 固定・採点キャッシュ) | 正答率 ≥ 0.75 (偶然 0.5) |
| M4 | **同一化の検出** | 同一議論内で代理 A と B の発話を埋め込み、A-B 間の平均 cosine と「A と本人 A の held-out」の cosine を比較 | A-B 間 < A-本人 間。ラウンドが進んでも A-B 間が 0.05 以上上昇しない |
| M5 | **根拠縛り率** | 代理の各論拠 (`Ground.data`) が evidenceIds を持つ割合 / evidenceIds の本文と主張が一致する割合 (判定 LLM) | 根拠付き ≥ 0.80、不一致 ≤ 0.05 |
| M6 | **誤帰属率** | 「推論」ラベルが無いのに本人発言に無い命題を断定した割合 | ≤ 0.05 |

held-out 発言の本文は M3 でしか要らないので、契約では `heldOut[].text` を**評価用の別エンドポイント/別ファイル** (議論には載せない) に分ける。

### 既存枠組みの流用

- `scripts/eval-retrieval.ts` (ゴールデンセット + `--judge` + 採点キャッシュ): 構造を流用して `eval:proxy` を作る。
  ゴールデン = 契約データ自体 (人物 × ジャンル × heldOut)。
- `gemma-ft.md` §9-4 の blind 判定: M3 にそのまま。
- `voice-rag-hybrid.md` の教訓: M1〜M3 は文字列一致ではなく**ラベル一致**で測る。

### 合格の最低ライン (ship 判定)

M1・M3・M5 を必須、M4・M6 を警告扱い。M1 でベースラインに勝てない profile は `eval_status='failed'` としてキャストから除外する。
**ベースラインに勝てないなら代理の意味が無い**ので、評価は機能の一部として Di に実装する (後付けにしない)。
評価は**取込側 (Di) の責務**: Histrio の出力が契約に合っていても「本人らしいか」は議論エンジンと組で測るしかない。
結果は Histrio の `persona_feedback_log` へ還流できる (Histrio の heuristic 学習の入力。後段)。

---

## 7. §4-6 議論の形

### 判定: **条件付き成立** (既存 dialectic に「憑依」拡張として載せる。別フローにしない。立場の割り方と根拠縛りを変える)

| 案 | 長所 | 短所 |
|---|---|---|
| A. dialectic に possession 拡張で載せる (推奨) | 論拠の応酬・トゥールミン検査・論破/止揚/結論なし・引用原文・ペーパー・未来予測が全部そのまま使える。`FlowPersona.possession` と `flow_session_persona.possession_name` が既にある | pro/con 固定と「立場を守る向きで答えよ」が本人の立場と衝突しうる |
| B. 別フロー (自由対話) | 本人らしさを出しやすい | 収束・結論・捏造防止を作り直し。質が構造で担保されない |
| C. rounds エンジン | 単純 | 同一化が最も起きやすい (状態が生テキストのみ) |

### A で変える 3 点

1. **立場はデータから導く**: `specifiedStance` (index で pro/con) を使わず、代理ごとに論点に対する立場を
   **立場カード + design_gap から判定** (小モデル・温度 0・1 call、`location=proxy-stance`)。「どちらでもない」なら
   `opinion` ロールで参加 (無理に陣営に入れない)。2 人が同じ陣営なら「同陣営内の差」として記録し warn
   (強制的に反対側へ回さない。本人と逆の立場で戦わせるのが最大の誤帰属)。
2. **根拠縛り**: `exchange-prompt.ts` の指示に「データ (data) は同梱した本人の発言 [E1..En] の ID を引く。引けない場合は
   `inferred: true` を付け、text を推測の言い方にする」を足す。`rule-check.ts` のコード検査に **`unsupported_attribution`**
   (evidenceId が束に無い / `inferred` 無しで本文に無い命題) を足し、違反は既存どおり `question` に格下げ。
   「本人が言っていないこと」は**議論の勝敗に効かない**。
3. **露出面での区別**: 発話に根拠付きは `[E3]` 参照、推論は `(推測)` を付ける。`FlowUtteranceRecord` に `evidenceIds[]` と
   `inferred` を足し `flow_utterance` に永続 (結論 md にも出す)。

### 人数と対戦形式

基本は **1 対 1 + 進行役**。roster で代理ごとに別モデルを割れる (同一モデルの同一化緩和、`dialectic.md` §5-4)。
3 人以上は M4 が通ってから。

### 議題との関係

代理が発言していないゲームを議題にする場合 (主用途:「この新作仕様を A と B はどう評価するか」)、design_gap の B 案を
議論開始時に Di が計算してペーパーの `# 代理の前提` 節に載せる。**代理のデータは契約から変えない** (議論ごとの再抽出はしない)。

### Histrio のロールモデルエンジンとの関係

role-model-engine.md §5 は「討論用ロール (論者 A/B・司会・書記) を namespace `di` で追加できる」とする。
代理議論では**ロール (振る舞いの型) は dialectic の構造 (debater / opinion / facilitator と立場) が決める**ので、
Histrio のロール選択関数は使わない。使うとすれば脚本層のプロファイル (エゴグラム等) を**口調メモの補助**として
`styleNotes` に写す程度 (§10)。状況カテゴリの指定 (発散→収束→対立) は dialectic の位相と重なるが、代理議論では不要。

---

## 8. §4-7 倫理・個人データ

### 判定: **条件付き成立** (既存方針の範囲で成立するが、線引きが 3 つ要る。Histrio が別サービスになることで契約で縛れる)

| 既存方針 | 本件での扱い |
|---|---|
| 出所は透明 / 個人は仮名 (`論者#xxxxxx`) | 維持。代理の表示名は Di が契約の `speakerKey` から一方向 hash で導く (`maskedPersonaLabel` と同型)。契約に表示名を含めない |
| 匿名 workspace、編集者名・アカウント名を保存しない | 維持。契約の受理条件で担保 (§10: 表示名・raw ID・URL を含む行は拒否) |
| 生 ID 参照は admin のみ | 生 ID は **Histrio 側にしか無い**構成にする (Di は仮名キーのみ受け取る) |

### 追加で必要な線引き

1. **対象の範囲を「公開発言の仮名話者」に限定**。Discord ユーザ・実名人物をターゲットにしない。
   契約の `speakerKey` 形式 (`ext:<source>:<hmac16>`) を Di が検証し、それ以外を拒否することでコードで担保する。
   Histrio が Concordia 向けに持つ会話 persona (namespace `cc`) や、Cc のアンビエント傍聴で拾う**人間の発言は
   代理の材料にしない** (namespace `di` の証拠ペルソナは公開レビュー由来のみ)。
   「代理戦争」の名で**特定個人への攻撃に使える設計にしない**のが最大のリスク回避。
2. **出所の露出縮小**: 既存方針は「1 件の声の出所」想定で、**同一人物の発言を 6〜10 件束ねて出す**のは再特定の強度が違う。
   推奨: 契約の evidence は `sourceKind` + `gameTitle` までとし **個別 URL を Di に渡さない** (Di には再特定手段が無い状態にする)。
   URL が要る監査は Histrio 側で `evidenceId` から引く。これは既存「出所は透明」の縮小なので人間判断事項 (§13)。
3. **推論と引用の分離** (§7-2・3)。
4. **オプトアウト**: 契約の `revoked: true` を Di が受けたら該当 profile と persona を `archived=1` にする (既存フラグ)。

### 成立しない条件

- 代理に本人の表示名や推定属性 (年齢・国) を付ける構成。契約に載せない。
- 議論結果を「○○ (本人) の意見」として外部公開 (GLAB 等) する運用。公開時は「公開発言から作った代理の発話」と
  明記する (既存 `publication` 承認ゲートに項目を足す)。

---

## 9. §4-8 責務境界 — Voluptas ↔ Histrio ↔ Di の三者

### 判定: **成立** (三者に分けると persona-pool.md の「Di にペルソナ生成を置かない」は改訂不要で、むしろ強化される)

| 責務 | Voluptas | Histrio | Discutere |
|---|---|---|---|
| 入力 | 同意済み本人のアンケート・プレイログ・感想 | (既存) Cc の seed + signals / (新規) 公開発言の JSONL (`steam-persona/` の出力) + ゲーム→ジャンル辞書 | 両者の出力契約 |
| 個人データ | 実名・認証 (Cernere) を持つ唯一の場所 | 公開 ID (SteamID64 等) を**内部だけ**で持つ。出力は HMAC 仮名 | 仮名キーのみ。生 ID・表示名・URL を持たない |
| 人物像の生成 | 質問票・証拠パイプライン (v2) | (既存) heuristic 会話 persona / (新規) 公開発言からの**証拠ペルソナ** (affect / gap / 代表発言 / 立場カード / 口調メモ)。**persona 正本** | **しない**。descriptor の決定的な言語化とクランプのみ |
| 出力 | ブリッジ v1/v2 (`ext:voluptas:<hmac16>`、affect + axes…) | `GET/POST /v1/personas` namespace `di` (本書 §10 の契約) + `/v1/render` (声色描画) | 議論・結論・評価レポート |
| 評価 | 母集団レポートを Di から受け取る (既存 §3) | 契約の充足度 (件数・正負) を自己申告。Di の評価結果を `persona_feedback_log` に受ける (後段) | 本人らしさ (M1〜M6) を測り `eval_status` を持つ |
| 既存コードの行き先 | 変更なし | `persona-adopt.ts` の集約・判定・`design-gap.ts` の差分・`voice-search.ts` の rerank を**形成層として移植** | `steam-persona/` (収集) は Di か Canalis に残し JSONL を Histrio に渡す / `persona-import.ts` と同型の取込、`persona-pool.ts`・`persona-descriptor.ts`・dialectic 拡張 |

### persona-pool.md との関係

「Discutere に…回答からのペルソナ生成…を置かない」は**そのまま**。本件は「Di は Vo に加えて Histrio (namespace `di`) からも
取り込む」の追記で済む。persona-chat-engine.md §3 が「Di 既存のペルソナエンジン v2 との統合方針は Di 側 spec で確定する」と
しているので、その Di 側 spec が本件になる。
C1 採用 (`persona-adopt.ts`) は **Di 内に残すか Histrio へ移すか**が判断事項 (§13): Histrio が同じ公開発言から形成するなら
二重集計になるため、長期的には **Histrio へ移し、Di の C1 は契約取込に置き換える**のが筋 (短期は共存可: `origin='adopted'` と
`origin='histrio'` を分ける)。

### Vo ブリッジとの二重登録防止

同じ人物が Vo (同意済み本人) と Histrio (公開発言) の両方から来うる。既存の `source_speaker_id` と `user_id` の
upsert 規約 (`upsertPoolPersonaBySpeaker` が imported 行に speaker を追記する) をそのまま使い、**affect は Vo を正本**
(persona-pool.md 既定)、**ジャンル別 profile は Histrio を正本**とする。

---

## 10. 論点 10: Histrio が持つべきペルソナ形成の範囲と Di への出力契約

### 判定: **条件付き成立** — Histrio の現 spec には無い「証拠ペルソナ」を足す。heuristic 原則とは**立場カードの抽出だけ**が抵触し、
決定的版を既定・LLM 補助を opt-in にすれば両立する。

### Histrio の現 spec との差分

| 観点 | 現 spec (persona-chat-engine / role-model-engine) | 代理議論に要るもの | 両立 |
|---|---|---|---|
| persona の源泉 | seed + signals (Cc の会話) の heuristic 生成 | **実在話者の公開発言** (レビュー) からの集計 | 新規の形成層 (`evidence persona`) を namespace `di` に足す。seed 生成とは別の入口 |
| LLM | 不使用 (描画の中央 Haiku を除く) | affect / gap / 選抜 / 口調メモは決定的に作れる。**立場カードだけ**は LLM 抽出が精度上有利 | 既定は決定的版 (下記)、`llmAssist` opt-in で Haiku 抽出 (Vo v2 §4.4 と同じ発想、provenance に `extractor: 'llm'`) |
| プロファイル語彙 | エゴグラム / ドライバー / 構え / 傾向 (脚本層) | 20D affect (sentiment-core) / aspect 別 gap / 発言 ID | 併存。脚本層は公開発言からは導けない (seed 用) ので証拠ペルソナでは**任意**。あれば口調メモに写す |
| 学習 | `persona_feedback_log` を重みに畳み込む (緩慢) | Di の評価結果 (M1〜M6) を還流 | 同じログに `source: 'di-eval'` で入れる (後段) |
| 記憶の読み書き | `GET/POST /v1/personas` | 同じ API で namespace `di` のジャンル別 profile を返す | API を流用、payload を契約で定義 |

### 立場カードの決定的版 (既定) と LLM 補助版 (opt-in)

- **決定的版**: 代表発言を文に分割し、aspect 活性が高い文 (`textToVector` の aspect 次元が閾値超) を「主張」として採り、
  その文の属する発言 ID を evidenceId にする。主張 = **本人の文そのもの** (要約しない)。精度は低いが捏造ゼロ。
- **LLM 補助版**: Haiku に代表発言から主張 3〜6 件を JSON 抽出させ、evidenceIds の無い主張を捨てる。要約されるぶん
  議論で使いやすい。Histrio の heuristic 原則に対しては「描画の中央 Haiku を形成に 1 回だけ使う opt-in」として spec に明記する。
- どちらも契約の `stanceCards[].extractor` に `'rule' | 'llm'` を残す。Di の評価 (M5) で両者を比較できる。

### 役割 (何を集め、何を出すか)

| 何を集めるか | 何を出すか |
|---|---|
| 公開発言 (レビュー・コメント)。入力は `steam-persona/` の JSONL (既に Histrio 向けに設計済み)。**期間**: 直近 24 か月を既定 (`observedMonths` を出す) | 人物 × ジャンル の profile (下記)。**全発言は出さない** |
| ゲーム → ジャンルの辞書 (Histrio 側の静的辞書) | 使った辞書の版 (`genreSpecVersion`) |
| 発言の極性ラベル (votedUp 等の signal) | gap・立場カード・held-out ラベル |
| 埋め込み (選抜用。Di に渡さない) | 代表発言の原文抜粋 (≤ 12 件) |

### 出力契約 (`GET /v1/personas?namespace=di&genre=…`、1 要素 = 1 人物 × 1 ジャンル。v1 案)

```jsonc
{ "contractVersion": 1,
  "speakerKey": "ext:steam:3f9a…(hmac16)",     // 仮名。生 ID・表示名・URL を含めない (Di が形式検証)
  "genre": "action-roguelike",                 // Histrio 辞書の id
  "genreSpecVersion": 1,
  "window": { "observedMonths": 18, "generatedAt": 1761177600000, "expiresAt": 1763769600000 },
  "sufficiency": { "utterances": 23, "positive": 14, "negative": 9, "games": 6, "confidence": "high" },
  "affect": { "vector": [/*20*/], "vectorSpecVersion": 1 },          // sentiment-core と同空間
  "designGap": { "vector": [/*20*/], "method": "pos-minus-neg",       // §4 A 案。作れなければ null + reason
                 "wants": ["asp.replayability", "asp.difficulty", "emo.anticipation"],
                 "complaints": ["asp.content", "asp.price_value", "asp.story"] },
  "stanceCards": [ { "id": "S1", "claim": "周回前提の設計は報酬の間隔で決まる", "polarity": "pos",
                     "evidenceIds": ["E2","E5"], "extractor": "llm" } ],
  "evidence": [ { "id": "E1", "text": "…原文 ≤ 400 字…", "sourceKind": "steam", "gameTitle": "…", "polarity": "neg",
                  "postedAt": 1750000000000 } ],                       // ≤ 12 件。URL なし (§8-2)
  "styleNotes": ["一人称は『自分』", "文末は『〜かな』が多い", "句点が少ない"],   // 頻度集計。脚本層があれば併記
  "likedGames": ["slug-a", "slug-b"],          // §4 B 案の材料 (Di の data/games と照合)
  "heldOut": [ { "game": "slug-c", "recommended": true } ],   // 本文は別エンドポイント (評価専用、議論に載せない)
  "revoked": false }
```

- `affect` / `designGap` の数値は Di が**言語化と選定にだけ**使う (prompt に数値は出さない、§3)。
- `sufficiency.confidence` が `low` の行は Di が取り込むが**キャストに出さない** (Vo v2 の「データ不足を 0 と区別する」と同じ思想)。
- `expiresAt` は Di の `persona-history-evidence.ts` (期限付き派生プロファイル) と同じ扱い (期限切れは議論候補から自動除外)。
- **圧縮の分担**: 選抜・抽出・材料づくりは Histrio、日本語化とトークン予算クランプ・議題ごとの並べ替えは Di (§5)。
- バッチ取込 (JSONL ファイル) も同じ payload で受ける (`persona-import.ts` の `--url` / ファイル upload と同型)。

### 既存コードの流用元としての評価

| 既存 | Histrio への流用 | 評価 |
|---|---|---|
| `src/crawler/steam-persona/` (横断投稿者の検出・集中収集・JSONL 出力) | **収集は Di (または Canalis) に残し、出力 JSONL を Histrio の入力にする**。出力コメントが既に「Vo / Histrio が読む中立な JSONL」 | 最有力。Histrio にクローラを持たせない (persona-chat-engine.md は Histrio をプラットフォーム接続なしの頭脳として設計) |
| `src/flow/persona-adopt.ts` (`evaluateSpeakers`: ゲーム単位連結 → textToVector → 極性・gap 判定) | 集計アルゴリズムを Histrio へ移植 (sentiment-core は共有) | 流用可。ジャンル単位の分岐を足す |
| `src/flow/design-gap.ts` (`computeDesignGap`) | 同上 | 純関数なのでそのまま |
| `src/discatier-engine-adapter/voice-search.ts` (RRF + MMR) | 代表発言の選抜 | 流用可。speaker 絞りの候補集合に変える |
| `src/flow/persona-descriptor.ts` | **Di に残す** (契約 → 日本語化は Di の都合) | — |

---

## 11. 論点 11: 境界 — Histrio = 形成と正本 / Di = 議論、と `/v1/render` の使い分け

### 判定: **成立** — 基準案 (Histrio = 人物像の形成と正本、Di = 受け取って議論させる側) は妥当。
**論証の生成は Di、声色の描画だけ Histrio `/v1/render` を任意で使う**。

### 基準案の妥当性 (置き場所の比較)

| 案 | 長所 | 短所 |
|---|---|---|
| A. Histrio = 形成と正本 / Di = 議論 (基準案、推奨) | 生 ID を Histrio 内に閉じ込められる。Vo と同じ「ブリッジ」構図で persona-pool.md の境界を変えずに済む。Histrio の README が既に「Di の討論・議論ペーパー生成に persona/描画を提供する」を掲げている。Cc 停止時も Di 向け API 単体で動く設計 (受入基準) | Histrio にコードが無い (bootstrap から)。sentiment-core / ジャンル辞書を共有する配線が要る。Histrio の heuristic 原則と立場カード抽出の折り合い (§10) |
| B. Di 内モジュール (`src/flow/proxy/` に形成層も置く) | 最短。既存 `steam-persona/` と `persona-adopt.ts` をそのまま育てる | Di が生 ID を持ち続ける (束ねた発言で再特定強度が上がる)。「Di にペルソナ生成を置かない」との緊張。Histrio の README と二重化 |
| C. Canalis に形成層を置く | 取得層の再利用が最大 | Canalis は「取ってくる」ライブラリで、人物単位の状態 (HMAC 鍵・期限・学習) を持つ責務が無い |
| D. Voluptas | 証拠パイプライン v2 の集約器を再利用 | Vo は同意済み本人が対象。公開 ID を持ち込むと匿名方針と Cernere 単一情報源の両方に逆行。却下 |

**推奨 A の進め方**: 契約 (§10) を先に固定し、Di 側 (取込・束・議論・評価) は契約サンプルで先に完成させる。
Histrio の bootstrap (`spec/tasks/2026-08-12-histrio-bootstrap.md`) が未着手のため、**短期は Di 内に契約生成器を仮置き**
(`scripts/proxy-contract-from-kg.ts`、既存 `persona-adopt-runner.ts` の集約を使う) して契約サンプルを実データから作り、
Histrio 骨格ができ次第その生成器を Histrio へ移す (切り出し時に契約が固定されているのが条件)。

### `/v1/render` を代理の発話に使うか、Di の persona-turn で描くか

| 観点 | Histrio `/v1/render` (persona id + intent + 文脈 → 1 発話、中央 Haiku) | Di `persona-turn` / `exchange-prompt` (既存) |
|---|---|---|
| 出力の形 | 自然文 1 発話 (stateless) | **構造化 JSON** (`act / attack / kind / data / warrant / text`) + 口語 text。rule-check・論証グラフ・根拠状態がこの JSON に依存 |
| 根拠縛り | 契約の evidence を文脈として渡せるが、`evidenceIds` を返す契約が無い | prompt に [E1..En] を入れ、`evidenceIds` / `inferred` を JSON で返させ `rule-check` で検査できる |
| モデル | Haiku 固定 (描画) | roster で代理ごとに別モデル (同一化緩和) |
| 一貫性 | Cc の会話と同じ声色 | 議論ごとの立場・論証状態と一体 |
| コスト | 1 発話 +1 call | 既存どおり |

**結論**: 論証 (何を主張し、どの発言を根拠にするか) は **Di の既存経路で生成**する。`/v1/render` は
**text (Discord に流す口語) の声色整形に限定した opt-in の後段** (`flow.proxy.renderVia = 'histrio' | 'none'`、既定 `none`)。
使うときも入力は Di が確定した `text` と `styleNotes` で、**主張・根拠は変えさせない** (整形後の text に含まれる命題が
元 text と変わっていないかは判定 LLM で検査しない = 口語の言い換えのみを頼む prompt にし、`data` / `warrant` は render に渡さない)。
Histrio 停止時は `none` に degrade (議論を止めない)。

---

## 12. §4-9 実装の分割と受入条件

### 判定: **成立** (Di 側 5 段 + Histrio 側 2 段。契約サンプルを先に固定すると両側を独立に進められる)

| 段 | 側 | 内容 | 受入条件 (テストで確かめられる形) |
|---|---|---|---|
| **D0 契約** | 両 | Di `spec/interface/histrio-persona.md` (契約 v1、§10) + サンプル (`data/eval/proxy-contract.example.jsonl`)。Histrio 側は `spec/feature/evidence-persona.md` (新規) に同じ契約を載せる | ① zod スキーマで検証できる ② 表示名・raw ID・URL を含む行が拒否される ③ サンプルが Di の全テストの fixture になる |
| **D1 取込** | Di | `src/flow/proxy/import.ts` (契約検証・`flow_persona` upsert (origin='histrio')・`flow_persona_profile` migration・`revoked`・`expiresAt`・`--url` pull とファイル upload) | ① 同じ (speakerKey, genre) の再取込が行を増やさない ② `revoked` で profile と persona が archived になる ③ 期限切れ profile が一覧に出ない ④ profile 行に表示名・authorId・URL が無い (スキーマテスト) |
| **D2 束** | Di | `src/flow/proxy/bundle.ts` (言語化 descriptor / gap 言語化 / 予算クランプ / 文区切り切断 / 議題で evidence 並べ替え) | ① 入力がどれだけ大きくても出力 ≤ 3,500 トークン相当 ② evidence ID と本文の対応が一意 ③ 数値ベクトルが束の文字列に含まれない ④ `confidence=low` は束を作らない |
| **D3 議論接続** | Di | `src/flow/proxy/cast.ts` (stance 判定・opinion フォールバック・同陣営 warn) / `exchange-prompt.ts` の根拠指示 / `rule-check.ts` の `unsupported_attribution` / `flow_utterance` の evidenceIds・inferred / 露出面 / `renderVia` (Histrio 整形、既定 none) / Discord 観点タグ `代理` + starter「代理: 論者#a, 論者#b」/ Web `/flow` キャスト指定 | ① 「どちらでもない」の代理は debater にならない ② evidenceId が束に無い反論は `question` に格下げされ根拠が challenged にならない ③ `inferred` の発話が結論生成の入力で「推測」と区別される ④ Histrio 不通で `none` に degrade し議論が止まらない ⑤ 非代理議論の出力がバイト等価 (回帰) |
| **D4 評価** | Di | `scripts/eval-proxy.ts` + `src/flow/proxy/eval/` (M1〜M6・judge キャッシュ・`eval_status`) | ① 固定サンプル + モック LLM で結果が決定的 ② M1 のベースラインが多数派で計算される ③ `eval_status=failed` の profile が D3 のキャストから除外される |
| **H1 契約生成器 (仮置き)** | Di → Histrio | `scripts/proxy-contract-from-kg.ts` (`persona-adopt-runner` の集約 → ジャンル辞書 → gap → 選抜 → 立場カード (決定的版) → held-out → 契約 JSONL)。Histrio 骨格ができたら移す | ① 固定の話者意見群から `designGap` が決定的に同じ値 ② 正負各 2 件未満は `designGap=null` + reason ③ evidenceIds が入力に無い立場カードは捨てられる ④ `--dry` で「条件を満たす人物 × ジャンル数」のレポート |
| **H2 Histrio 実装** | Histrio | bootstrap (既存 task) の後、証拠ペルソナ形成層 (H1 の移植) + `GET/POST /v1/personas` namespace `di` + `llmAssist` opt-in + `/v1/render` 整形 + `persona_feedback_log` への Di 評価の受け口 | ① Di 側テストが契約サンプルのまま通る ② Di のコードに生 ID 経路が残らない ③ Histrio の heuristic 受入基準 (LLM 追加呼び出しなし) が `llmAssist=false` で満たされる |

### 順序と理由

D0 → (D1 → D2 → D4 の骨組み → D3) ∥ (H1) → D4 実測 → H2。**D3 (Discord に出す) は D4 の M1/M3/M5 が合格基準を
満たした profile だけに限定**する。実 LLM を使う D4 実測・H1-④ はユーザ指示後に行う (本委託の規約)。

### ロールバック

全て opt-in (`flow.proxy.enabled` 既定 false、観点タグ `代理` が無ければ従来経路)。migration は追加のみ。契約は版付き。

### コスト見積 (1 代理 × 1 ジャンル)

| 処理 | 回数 | 側 / モデル |
|---|---|---|
| 立場カード抽出 | 1 (再集計時に差分) | Histrio / 決定的 (既定) or 小モデル (opt-in) |
| 立場判定 (議論開始時) | 論点数 (3〜5) | Di / 小モデル・温度 0 |
| 発話 | 既存 dialectic と同じ (1 call の prompt が +3.5k) | Di / メイン |
| 声色整形 (opt-in) | 発話数 | Histrio / Haiku |
| 評価 (M1〜M6) | held-out 件数 + 判定 | Di / 小モデル、キャッシュ |

FT と違い、人物が増えても学習ジョブは発生しない。

---

## 13. 未決事項 (人間にしか決められないもの、推奨つき)

| # | 事項 | 推奨 |
|---|---|---|
| 1 | **対象の範囲**: 公開発言の仮名話者に限定するか (Discord ユーザ・実名人物・Cc の会話 persona は対象外) | **限定する**。限定しないなら §8 は成立しない |
| 2 | **出所 URL を Di に渡さない** (§8-2、既存「出所は透明」の縮小) | **渡さない** (監査は Histrio 側で evidenceId から引く) |
| 3 | **10k の単位**: 「1 ターンに載せる代理データ ≤ 7k (1 人 3.5k)」「Histrio 材料 ≤ 10k 件/ジャンル」の 2 本立て | **よい** |
| 4 | **Histrio の heuristic 原則と立場カード**: 決定的版を既定、LLM 補助を opt-in でよいか | **よい** (Vo v2 §4.4 `llmAssist` と同型) |
| 5 | **短期の仮置き**: Histrio 骨格ができるまで契約生成器を Di 内に置く 2 段構えでよいか | **よい** (契約固定が条件) |
| 6 | **Di の C1 採用 (`persona-adopt.ts`) の行き先**: Histrio へ移すか共存か | 長期は**移す** (二重集計回避)。短期は `origin` で分けて共存 |
| 7 | **`/v1/render` の用途**: 声色整形の opt-in に限定 (論証は Di) | **限定する** |
| 8 | **同陣営になった代理の扱い**: 強制的に反対側へ回さない | **回さない** |
| 9 | **外部公開** (GLAB 等) で代理発話を出すか | 当面 **出さない**。出すなら「公開発言から作った代理」の明記を承認項目に足す |
| 10 | **名称**: 「代理戦争 AI」を外向きの機能名にするか | 内部コードは `proxy`、露出面は「代理議論」等の穏当な名前 (攻撃用途の連想を避ける) |

---

## 14. 前提未確定 (本書で確かめられなかったこと)

- Histrio はコードが無く (main は初期化コミットのみ)、契約 v1 (§10) と「証拠ペルソナ」の追加は本書の案。Histrio 側 spec への反映は別 PR。
- Histrio `/v1/personas` / `/v1/render` の payload は現 spec に無く、本書の契約は Di 側からの提案。
- 実データで「人物 × ジャンル ≥ 8 件、正負各 ≥ 2」を満たす人物数 (H1-④ で測る)。
- `lib/lapilli` (sentiment-core) はこの worktree に checkout されておらず、次元定義は `SENTIMENT.md` と `design-gap.ts` から読んだ。
- `spec/tasks/2026-08-11-ft-distill-eval.md` は存在せず、blind 勝率は `gemma-ft.md` §9-4 を参照した。
- 日本語トークン換算 (1.3 トークン/字) は概算。D2 の予算クランプは文字数で実装し、実測で係数を直す。
