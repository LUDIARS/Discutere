---
task: persona-game-archetype-context
project: Discutere
kind: 設計
author: Opus (Cc セッション、neco 指示 2026-11-23)
created: 2026-11-23
status: decided (D1〜D3 は 2026-11-23 neco「全部推奨どおり」。A1 は #4086 でマージ済み。D3 は立場を人物像から導く方式とセット)
related:
  - spec/feature/flow/dialectic.md
  - spec/feature/flow/persona-pool.md
  - spec/feature/flow/proxy-debate.md
  - actio:af313176-0e6b-4b77-a082-ce1a14a2375b
  - E:/Document/Ars/Histrio/spec/feature/virtual-persona-life-script.md
  - E:/Document/Ars/Ludus/spec/data/game-template/README.md
---
# ペルソナとゲームアーキタイプを前提に持った発言 — 設計

## 0. 依頼

neco (2026-11-23):「Histrio を使ってペルソナとゲームアーキタイプもコンテクストを持った発言をする」。
議論の各発言者が、(a) 自分の人物像 (どんな遊び手か) と (b) 議題のゲームの型 (どういうジャンル・コアループか) を
前提にして話すようにする。

## 1. 現状 (e796c5a 時点の調査)

| 材料 | 持っている場所 | dialectic の発話 prompt に載るか |
|---|---|---|
| 名前・特徴 (traits)・話し方 | `FlowPersona` | 載る (`dialectic/turn_prompt.cpp` `build_dialectic_turn_prompt`) |
| 価値軸 (`valueAxis`) | persona-setup が LLM で生成 | 載る |
| 遊び手の型の軸 (`preferenceAxes`: mtg.timmy/johnny/spike, style.achiever/explorer/…)・苦手 (`aversions`)・メカニクス反応・年代/課金 | プール (`flow_persona`)。`persona_descriptor.cpp` が言語化 | **載らない**。憑依 (`apply_theme_possession`) は opinion 枠 1 人だけで、descriptor を使うのは rounds エンジンの `build_persona_user_prompt` だけ。dialectic は `possessionName` を表示に使うだけ (`dialectic/driver.cpp:229`) |
| ゲームの型 | Di には無い。Ludus `spec/data/game-template/<genre>/` (17 ジャンル: 概要・コアループ・必須機能・ux) と `game-lexicon/genres/*.toml` | **載らない**。ペーパー本文 (system) にメカニクス表はあるが、ジャンルとしての型 (何があるとこのジャンルか・遊び手が何を期待するか) は無い |
| 人生脚本パターン (Histrio の仮想人格) | Histrio の設計書だけ (コード無し) | — |

結論: 「どんな遊び手が、どんな型のゲームについて話しているか」は、既定の dialectic エンジンでは発話 prompt に一切届いていない。

## 2. 方式

### 2.1 2 つの文脈ブロック

発話 prompt (`build_dialectic_turn_prompt`) と定立 prompt に、次の 2 ブロックを足す。

1. **人物像ブロック** (発言者ごと、`# あなたの遊び手としての型`)
   - 遊び手の型: `preferenceAxes` 上位 2〜3 軸を言語化 (例「大きな体験と探索を求める。勝利と最適化には関心が薄い」)
   - 苦手: `aversions` 上位 2 件、年代・課金
   - 人生脚本パターン (Histrio 接続後): パターン名 + 会話反応の 1 行 (例 `until`「条件が揃うまで保留し、前提確認を先に求める」)。
     エゴグラム等の従属項は口調の 1 行にだけ使う (立場には効かせない。Histrio 仕様 §1 と同じ)
   - 「この型の人として、論点についてどう感じるかを根拠と一緒に言う。型を名乗らない」を指示に入れる
2. **ゲームの型ブロック** (議論ごと、ペーパー system の末尾 `# ゲームの型`)
   - 議題のゲームが当たる Ludus ジャンル id (複数可、最大 2) と、各ジャンルの「コアループ」「これがあるとこのジャンル」「遊び手が期待する体験 (ux)」の要約
   - system に置くので議論中は固定 (プロンプトキャッシュを壊さない)

### 2.2 キャストの選び方

- 賛成・反対・意見役の全員にプールの人物を割り当てる (今は opinion 1 枠だけ)。
  議題との近さ (`select_possession_by_theme`) の上位から、**遊び手の型の軸がなるべく違う人**を選ぶ
  (同じ型ばかりだと同じ意見に寄る — 代理議論の統合レビュー §0 の「同一化」と同じ懸念)。
- 立場 (賛成/反対) は今までどおりセッション開始時に固定する。型は「どう感じるか」の材料で、立場は変えない。

### 2.3 正本と境界

| 材料 | 正本 | Di の扱い |
|---|---|---|
| 遊び手の型の軸・苦手 | Di のプール (今の場所のまま) | 言語化して載せる |
| 人生脚本パターン | Histrio (`persona_profiles.script_pattern_id`) | `GET /v1/personas/:id/profile?namespace=di` で受け取る。Histrio 不通・未登録はその行だけ省く (議論は止めない) |
| ゲームの型 | Ludus game-template | 取り込みスクリプトで `data/game-archetypes.json` に要約を焼く (ジャンル id・コアループ・必須要素・期待体験、各 ≤300 字)。実行時に Ludus を読まない |
| 議題 → ジャンルの対応 | ペーパー | 草案作成時に `data/games` のゲーム名・ジャンルとキーワードで決定的に当て、ペーパー調整で人が直せる (`ジャンル: vampire_survivors_like`) |

- 代理議論 (proxy) は対象外 (本人の発言に縛るため、型からの推測を混ぜない)。
- `機密`/`内部` タグでも使える (外部の声ではない)。

### 2.4 予算

- 人物像ブロック ≤ 250 トークン/人、ゲームの型ブロック ≤ 600 トークン/議論 (system 側、キャッシュされる)。
- 型を載せた分の発話の重複 (同じ型の言い換え) は既存の重複禁止指示で抑える。

## 3. 実装の分割

| 段 | 内容 | 受入条件 |
|---|---|---|
| A1 | `persona_descriptor` を dialectic の発話・定立 prompt に配線 (全キャスト)。キャストの型の多様化 | descriptor の無い人は従来とバイト等価 / 型の違う人が選ばれる単体テスト |
| A2 | `scripts/build-game-archetypes` (Ludus → `data/game-archetypes.json`、生成物はコミット) + ペーパーのジャンル当て + system 末尾の `# ゲームの型` | 同じ入力で同じ JSON / 当たらないときはブロック無し / ペーパー調整で上書きできる |
| A3 (実装済み) | Histrio クライアント (`native/src/integrations/histrio_client`) と人生脚本パターン行 | Histrio 不通でも議論を続行 / 通信を 1 回あたり 1.5 秒で打ち切り、プロフィールは議論開始時に人物ごとに 1 回だけ取得 |

neco (2026-11-24)「Di の議論者はワンオフで作られた人格なので人格として扱うには不適切」「議論者のテンプレートを用意してけばいいんじゃないかな」。

A3 は使い捨てのプール参加者に版付きの議論者テンプレートを割り当て、`HISTRIO_URL` 設定時に `GET /v1/personas/<tpl-id>/profile?namespace=di` で型を照会する。同一テンプレートの照会は議論内で共有する。プロフィールと同じ ID・版の脚本を使い、カタログだけが取得できない場合は同じ ID の Di の写しを使う。組み合わせを解決できなければ Di が議論開始時に割り当てた脚本を維持する。テンプレート ID、採用した脚本 ID、カタログの版、出所を `flow_session_persona` に記録する。

Steam の公開レビューに基づく代理は人物パックの Histrio ID を照会し、ID やプロフィールが無ければ脚本を省略する。Histrio 側の Steam 人格取り込みは別タスク。実装参照は `actio:d4ddf2a8-8c2b-40b2-b8b2-0e722893d224`。

## 3.5 設計評価 (2026-11-23、3 者) の指摘

- 方向は 3 者とも妥当。ただし **アーキタイプは v2 の後** に効果を測る (先に入れると人物像の効果と結論の直しの効果を分けられない)。
  A1 は personaIds で指定したプール人物だけに効くので、既定のキャストの議論は変わらない。
- 【重大】立場と型を独立にしない: 型に反する立場を割り当てない、または立場を論点ごとに人物像から導く (代理議論の
  `proxy/stance` と同じ形)。D3 とセットで決める。
- 【重大】型に由来する感じ方は「推測」として出所を型付けする (声・資料と区別し、本人の観測済み属性として扱わない)。
- 【注意】効果の受入を配線の単体テストだけにしない: 代理議論の判別評価を流用し「伏せた発言がどの型の人物のものか当てられるか」を測る。
- 【注意】Ludus の game-template に運営型 (ライブサービス・ガチャ) のジャンルが無い点を D1 の前に確かめる。
- Histrio は「対話の振る舞い (人生脚本・口調)」、Di / Voluptas は「遊び手の嗜好」と責務を分ける (Neon)。

## 4. 人間判断 (2026-11-23 neco「全部推奨どおり」で確定)

| # | 事項 | 推奨 | 理由 |
|---|---|---|---|
| D1 | 「ゲームアーキタイプ」は Ludus の game-template (17 ジャンル) を正本にしてよいか | **はい** | 既にジャンルごとのコアループ・期待体験が書かれている。新しい分類を作らない |
| D2 | Histrio のサービス化 (S6) と人生脚本パターンの実装をいつやるか | **A1・A2 を先に Di で入れ、Histrio は別タスク** | Histrio はコードが無く、立ち上げ (Excubitor 登録・ポート・データ) が大きい。A1・A2 だけで「型を持った発言」の大半は届く |
| D3 | 全キャストをプールの人物にしてよいか (今は意見役 1 人だけ) | **はい** | 賛否役が型を持たないと、型の違いが議論に出ない |
