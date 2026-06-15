# ペルソナプール / 憑依 / 合成 / 壁打ち相手指定 / SDK 化 — 設計

> 状態: ドラフト (2026-06-15)。新フロー (src/flow) の persona モデル拡張。
> 関連: [OVERVIEW.md](./OVERVIEW.md) §4 (駆動・ロール), §5 (ペーパー), [[feedback_di_flow_llm_wiring]]。

ユーザ依頼 (2026-06-15) の 7 改修をまとめる。A/D は実装済み、共通基盤 (データ層) も実装済み。
B/C/E/F/G を本書で定義する。

## 0. 用語

- **ペルソナプール** (`flow_persona`): 学習データ別に用意 (C) / 合成 (F) した**永続**ペルソナ。
  各々 20 次元 affect ベクトル (sentiment-vector.ts と同空間) を持つ。
- **ユーザ嗜好** (`flow_user_affect`): ユーザ (Discord id 等) の「ゲームに望む感情/体験」ベクトル。
- **憑依 (possession)**: 議論の**投稿主体 1 体**に、嗜好 affect が最も近いプールペルソナをアサインする (B)。

## A. persona-engine 停止 — ✅ 実装済み

`personaEngine.enabled` (既定 false)。engine/bridge/facilitator/consensus/auto-seed を起動しない。
worker-pool LLM / classifier / peDb / core の構築は維持 (フローが依存)。

## D. ラウンド/ターン数の都度指定 — ✅ 実装済み

`runFlow` が `options.rounds/turnsPerRound` を優先 → なければ config 既定。上限クランプ (R≤10/T≤20)。
入力: WebUI 数値欄 / Discord starter 本文 `ラウンド:N ターン:M` (`parseRoundsTurns`)。

## 共通基盤 — ✅ 実装済み (`src/flow/persona-pool.ts`, migration `flow_0005_persona_pool`)

- `flow_persona` / `flow_user_affect` テーブル。
- `insert/get/list/archivePoolPersona`, `upsert/getUserAffect`, `selectByAffinity` (cosine 最近傍), `toFlowPersona`。

## B. 憑依 (投稿主体 1 体) — 未実装

1. ユーザ嗜好を登録: `upsertUserAffect(userKey, desiredText)` (text→20次元)。
   - 入力経路 (**要確認/既定**): Discord slash `/di-憑依` のモーダルで「望む体験」を文章入力。
     未登録ユーザは憑依しない (= 従来の生成キャストのまま)。
2. 議論起票時: 投稿主体の `userKey` で `getUserAffect` → `selectByAffinity(vector, 1)` で最近傍 1 体を取得。
3. その 1 体を `toFlowPersona` 化し、生成キャストの **opinion 役 1 枠を置換**して参加させる
   (facilitator + 残りは従来生成。Q3「投稿主体の1体だけ」)。
4. 配線: `runFlow` に `possessedPersona?: FlowPersona` を渡せるようにし、`generateFlowPersonas` 後に 1 枠差し替え。
   dispatch / discord-live / web routes が `userKey` を解決して注入する。

## C. ペルソナ生成ワークフロー (method TBD) — 未実装 (スキャフォルド)

- 生成 IF: `generatePersona(source): PoolPersona` (affect ベクトルは学習データ/メカニクスから導出 or LLM)。
  実生成ロジックは pluggable。当面のスタブ = 外部の声 (listExternalVoices) の感情平均 + LLM で人物像生成。
- 起動: `npm run persona:generate -- --source <learningSet>` (将来 admin コマンド)。
- プールが空なら憑依/壁打ち相手指定は従来生成にフォールバック。

## E. claude-p → SDK 化 + cache-control — 未実装

- **サブスクのまま SDK 可** (Q1): `AnthropicSdkClient` を OAuth 対応にする。
  `Authorization: Bearer <oauth>` + `anthropic-beta: oauth-2025-04-20` で `/v1/messages` に通る
  (claude-api skill)。`x-api-key` (従量) も併存。
  - トークン源 (**要確認/既定**): env `ANTHROPIC_AUTH_TOKEN`、無ければ `ant auth print-credentials --access-token`
    を spawn して取得 (短命なので呼出時に再取得 or TTL キャッシュ)。
- **cache-control**: ディスカッションペーパーの安定部 (テーマ/メカニクス/タグ補足) を `system` ブロックに置き
  `cache_control: {type:"ephemeral"}` を付与、ターン可変部 (前ラウンド要約・指示) を user メッセージへ。
  director / persona prompt / vote / summary / conclusion の invoke を system+prompt 構成に再編。
- フロー LLM: worker-pool 主、**フォールバックを claude-p → SDK(OAuth)** に。`usage` が返るので
  cost-logger のトークン計上が有効化 (現在 null)。

## F. ペルソナ合成 — 未実装

- 入力: 2 (以上) の親ペルソナ id。
- affect: 親ベクトルの (重み付き) 平均。traits/口調/名前: LLM で「ローグライク好き×ソシャゲユーザー」を
  融合した新人物像を生成。
- 保存: `insertPoolPersona({origin:"synthesized", parentIds:[...], ...})`。以後プールの一員として
  憑依/壁打ち相手に使える。
- 起動: `npm run persona:synthesize -- --parents a,b`(将来 UI/コマンド)。合成ペルソナの意見は
  通常の議論/壁打ちで聞ける。

## G. 壁打ち相手のペルソナ指定 — 未実装

- `SparringSession` に「相手ペルソナ」を渡せるようにする (現在は生成 2 体)。
- 指定経路: WebUI = プールからセレクト / Discord = starter 本文 `相手:<名前 or id>` をパースしてプール解決。
- 未指定なら従来通り生成。

## 実装順

1. E (SDK/OAuth/caching) — コスト/精度の即効。
2. B 憑依配線 + slash 入力。
3. G 壁打ち相手指定。
4. F 合成。
5. C 生成スクリプト (スタブ)。
