---
task: proxy-war-ai-review
project: Discutere
kind: 設計レビュー
created: 2026-10-23
branch: design/proxy-war-ai
---
# 代理戦争AI — 実現可能性・実効性レビュー依頼 (Fable / Astra 二本立て)

## 0. 進め方

- 同じ依頼文を Fable (design-hard-fable5) と Astra (astra-mid) に別々に出している。
- **もう一方の案は見ないこと** (ブランチ `design/proxy-war-ai-fable` / `-astra` を相互に読まない)。
- 親 (Opus) が 2 案を突き合わせて統合レビュー + 統合設計にまとめ、その後に実装する。
- この依頼では **実装しない**。設計書 (レビュー書) 1 本以外を変更しない。**PR を出さない**。

## 1. 人間の指示 (原文、要約しない)

> Diの拡張として、Vo関係なくペルソナ形成した人間同士を会話させる「代理戦争AI」システムを作ろうと思います。
>
> 本当はFTしたほうがいいんでしょうけど、特定のジャンルのdesign_gapと抽象化されたベクトルデータがあれば議論に必要なペルソナデータは軽量に収まると考えており、そのベクタライズデータを食わせるほうが高速で精度が高いと考えます。
> 特定ジャンルのデータは10k以内に収めるとして、ペルソナデータの生成と議論精度についての実現可能性と実効性をレビューし、実装します。
> レビューはOpusが整備→FableとAstraでレビュー→Opusがまとめる
> とします。

## 2. 親の解釈 (違うと思えば指摘すること)

1. **代理戦争AI** = 実在の人間 (の発言履歴) から作ったペルソナを「その人の代理」として AI に演じさせ、
   代理同士を議論させる仕組み。既存の議論フローの使い捨てキャスト (`src/flow/personas.ts`) や
   役割ペルソナ (推進派/懐疑者…) ではなく、**特定の実在人物の立場・嗜好を再現すること**が目的。
2. **Vo 関係なく** = Voluptas (アンケート・プレイログからのペルソナ生成) を経由せず、Di 自身が持つ材料
   (クロール済みの公開レビュー話者、Steam 横断レビュアー `src/crawler/steam-persona/`、
   公開レビュー話者の採用 `src/flow/persona-adopt.ts` = C1) でペルソナを形成する。
   ※ 現行 canonical の `spec/feature/flow/persona-pool.md` は「Discutere にペルソナ生成を置かない
   (Voluptas の責務)」と定めている。今回の指示はこの境界の変更を含みうる。境界をどう引き直すか
   (Di 内に置く / 別モジュールに切る / C1 採用の延長として読む) もレビュー対象。
3. **ペルソナデータの形** = 人物ごとに「ジャンル単位の design_gap (その人が望む状態 − 現状)」と
   「抽象化したベクトル (例: 既存の固定 20 次元 sentiment-core、または埋め込み)」を持ち、
   それを prompt に入れて LLM に演じさせる (= FT ではなく in-context 条件付け)。
   design_gap は既存の改善フロー `src/flow/design-gap.ts` (目標 − 現状 の 20 次元差分) の概念を
   人物単位に持ち込むものと解釈している。
4. **10k 以内** = 1 ジャンルあたりのペルソナデータ (prompt に入れる量) を約 10k トークン以内に収める
   意味と解釈した (10k 件 / 10KB の可能性もある。どれが妥当か、各単位で何が収まるかも評価してよい)。
5. **人間の仮説** = 「FT よりベクトル化データを食わせる方が高速で精度が高い」。この仮説を検証可能な形に
   して、成り立つ条件・成り立たない条件を示すことが主題。

## 3. 前提として読むもの (すべて絶対パス)

リポジトリ: `E:/Document/Ars/Discutere` (main 起点。worktree 内でも同じ相対パスで読める)

- 全体ルール: `E:/Document/Ars/Discutere/CLAUDE.md` (個人データ方針「出所は透明 / 個人は仮名」を含む)
- ペルソナプール・境界: `spec/feature/flow/persona-pool.md`, `spec/feature/flow/persona-bridge.md`
- 既存実装: `src/flow/persona-pool.ts` (20D 近傍選定), `src/flow/persona-adopt.ts` (公開レビュー話者の採用 C1),
  `src/flow/persona-descriptor.ts` (憑依 descriptor), `src/flow/persona-history-evidence.ts`,
  `src/flow/persona-cast.ts`, `src/flow/persona-turn.ts`, `src/flow/personas.ts` (PersonaPossession)
- design_gap: `src/flow/design-gap.ts`, `spec/feature/flow/improvement.md`
- ベクトル空間: `src/flow/sentiment-vector.ts` (= `@ludiars/sentiment-core` の固定 20 次元)
- 外部の声と埋め込み: `spec/feature/voice-rag-hybrid.md`, `src/discatier-engine-adapter/voice-search.ts`
- Steam 横断レビュアー: `src/crawler/steam-persona/`, `spec/domains/steam-persona.domain.json`
- 議論エンジン: `spec/feature/flow/dialectic.md` (論拠の応酬・止揚/論破/結論なし), `spec/feature/flow/OVERVIEW.md`
- FT 既存計画: `spec/feature/gemma-ft.md` (Gemma QLoRA 蒸留), `spec/tasks/2026-08-11-ft-distill-eval.md` (blind 勝率)
- 参考 (Voluptas 側のペルソナ設計、境界比較用): `E:/Document/Ars/Voluptas/persona-engine-v2-design.md`

## 4. レビュー・設計してほしいこと

独立に考えてよい。親の解釈と違う結論になってよい。各項目に **判定 (成立 / 条件付き成立 / 不成立)** と根拠を付けること。

1. **仮説の分解**: 「FT よりベクトル化データを食わせる方が高速で精度が高い」を、
   (a) 生成コスト・速度 (b) 人物の再現精度 (c) 議論の質 (d) 更新容易性 に分けて評価する。
   FT・in-context (ベクトル + design_gap)・RAG (発言の原文抜粋)・それらの併用を比較する。
2. **ベクトルを LLM に「食わせる」意味**: 数値ベクトル (20 次元の値) をそのまま prompt に入れて LLM が
   人物を演じ分けられるか。数値のまま / 言語化 (descriptor) / 代表発言の抜粋 のどれが効くか。
   埋め込み (bge-m3 等) は LLM が直接読めない点をどう扱うか。
3. **ペルソナデータの生成**: 実在話者の発言からジャンル別 design_gap とベクトルを作る手順。
   必要な最小発言数、ジャンルの切り方、現状/目標の決め方 (design_gap の「目標」は人物の何から取るか)、
   既存 C1 採用条件 (minOpinions 10・極性偏り除外・ゲーム間差分) で足りるか。
4. **10k 予算**: 1 人・1 ジャンルの prompt 量の内訳案 (ベクトル要約 / design_gap / 代表発言 / 口調)。
   2 人以上を同時に議論させたときの合計量と、既存のペーパー・外部の声と合わせた上限。
5. **議論精度の測り方**: 「代理が本人らしいか」をどう測るか。例: 本人の未使用発言 (held-out) に対する
   立場予測の一致率、blind 判定、代理同士の主張が人物間で区別できるか (崩れて同じ意見に寄らないか)。
   既存の `eval:retrieval` や FT の blind 勝率の枠組みを流用できるか。合格基準の数値案も出す。
6. **議論の形**: 代理同士を既存の dialectic (論拠の応酬) に載せるか、別フローにするか。代理の立場を
   守らせつつ、本人が言っていないことを捏造させない方法 (根拠を本人発言に縛る等)。
7. **倫理・個人データ**: 実在人物の代理を作ること自体のリスク (なりすまし・本人が言っていない主張の帰属)。
   既存方針 (外部 ID は仮名化・個人は露出しない) の範囲で成立するか。露出面での表記、本人特定の防止。
8. **責務境界**: persona-pool.md の「Di にペルソナ生成を置かない」との関係。どこに何を置くか。
9. **実装の分割と受入条件**: フルセットで実装するときの分割 (データ生成 / 保存 / 議論への接続 / 評価) と、
   各段の受入条件 (テストで確かめられる形)。

## 5. 成果物

- Fable: `spec/plan/2026-10-23-proxy-war-ai-review-fable.md`
- Astra: `spec/plan/2026-10-23-proxy-war-ai-review-astra.md`

章立て: 結論 (総合判定と一番の懸念 3 つ) → 解釈 (親の解釈への同意・異論) → 4 の 1〜9 それぞれ (判定・根拠・推奨)
→ 推奨する方式の詳細 → 実装の分割と受入条件 → 未決事項 (推奨つき、人間にしか決められないもの)。

- 実装しない。設計書以外を変更しない。
- 書き終えたら自分のブランチにコミットし、要点を 5 行で報告して終了指示を待つ。
- **PR を出さない**。

## 追加指示 1 (2026-10-23)。本文より優先

人間 (neco) の訂正 (原文):

> いえVoluptas（アンケートやプレイ記録から人物像を作るサービス）とは別の、
> ペルソナ形成用のデータマイナーみたいなやつを使用する想定です。

反映すること:

- §2-2 の「Di 自身が持つ材料 (既存クローラ・C1 採用) でペルソナを形成する」は親の誤読。
  ペルソナの材料は **Voluptas とは別の「ペルソナ形成用データマイナー」** から来る想定。
- データマイナーは現時点で実体が無い前提で扱う (既存の `src/crawler/steam-persona/` 横断レビュアー収集や
  `src/flow/persona-adopt.ts` は、その前身・流用元の候補として評価してよい)。
- 次をレビュー・設計の論点に加える:
  - (10) **データマイナーの役割と出力契約**: 何を集め (公開発言・ジャンル・期間)、何を出すか
    (ジャンル別 design_gap・抽象ベクトル・根拠となる発言の参照)。10k 予算に収める圧縮をマイナー側でやるか Di 側でやるか。
  - (11) **置き場所**: 独立サービス / Di 内モジュール / 既存リポ (例: Canalis クローラ層) のどれが妥当か。
    Di は「出力契約を受け取って議論させる側」に留める案を基準に比較する。
  - §4-8 の責務境界は「Voluptas ↔ データマイナー ↔ Di」の三者で書き直す。

## 追加指示 2 (2026-10-23)。追加指示 1 より優先

人間 (neco) の補足 (原文): 「役者みたいな名前のやつ」

- 「ペルソナ形成用データマイナー」は **Histrio** (ラテン語で役者) を指す。リポジトリ: `E:/Document/Ars/Histrio`
  (main は初期化コミット 60d929c のみ。spec だけでコードは無い)。
- 読むもの: `E:/Document/Ars/Histrio/README.md`, `E:/Document/Ars/Histrio/spec/feature/persona-chat-engine.md`
  (persona 正本・heuristic 生成・Di 向け `POST /v1/render` / `GET/POST /v1/personas` namespace `di`),
  `E:/Document/Ars/Histrio/spec/feature/role-model-engine.md` (交流分析の脚本層ベクトル + 状況層)。
- 追加指示 1 の (10)(11) は次の意味に読み替える:
  - (10) **Histrio が持つべきペルソナ形成 (データマイニング) の範囲と、Di への出力契約**。Histrio の現 spec は
    Concordia の会話 persona (seed + signals の heuristic、LLM 不使用) が中心で、実在話者の公開発言から
    ジャンル別 design_gap・抽象ベクトルを作る機能は書かれていない。足すなら何を足すか、heuristic 原則と両立するか。
  - (11) **境界**: Histrio = 人物像の形成と正本、Di = 受け取った人物像で代理同士を議論させる側、を基準案として
    妥当性を評価する。Histrio の `/v1/render` (描画) を代理の発話に使うか、Di の既存 persona-turn で描くかも比較する。
  - 責務境界 (§4-8) は「Voluptas ↔ Histrio ↔ Di」の三者で書く。
