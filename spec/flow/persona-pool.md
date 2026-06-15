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

## B. 憑依 (投稿主体 1 体・テーマ類推) — ✅ 実装済み

**嗜好/体験はテーマから類推** (ユーザ入力プロフィール不要)。
- 目標ベクトル = `textToVector(theme)` (`selectPossessionByTheme`)。
- `runFlow` 既定 `possess=true`: 生成キャストの **opinion 1 枠**をテーマ最近傍プールペルソナで置換。
  プール空 / 一致なしなら no-op (従来生成のまま)。投稿主体の代理は 1 体のみ (Q3)。
- `flow_user_affect` / slash は B では未使用 (将来「ユーザ別嗜好」用に残置)。
- TODO(改善余地): メカニクス `intended_affect` を目標ベクトルに加味する。

## C. ペルソナ生成ワークフロー (method TBD) — 未実装 (スキャフォルド)

- 生成 IF: `generatePersona(source): PoolPersona` (affect ベクトルは学習データ/メカニクスから導出 or LLM)。
  実生成ロジックは pluggable。当面のスタブ = 外部の声 (listExternalVoices) の感情平均 + LLM で人物像生成。
- 起動: `npm run persona:generate -- --source <learningSet>` (将来 admin コマンド)。
- プールが空なら憑依/壁打ち相手指定は従来生成にフォールバック。

## E. claude-p → SDK 化 + cache-control — ✅ 実装済み

- **サブスクのまま SDK 可** (Q1): `AnthropicSdkClient` を OAuth 対応にする。
  `Authorization: Bearer <oauth>` + `anthropic-beta: oauth-2025-04-20` で `/v1/messages` に通る
  (claude-api skill)。`x-api-key` (従量) も併存。
  - トークン源: **Claude Code 認証 `~/.claude/.credentials.json` の `claudeAiOauth.accessToken` を読む**
    (`refreshToken`/`expiresAt` も保持。Claude Code が背景で鮮度維持)。呼出時に再読込 + `expiresAt` 確認の
    TTL キャッシュ。期限切れ & 再読込でも失効なら SDK は ok:false → claude-p フォールバックに落とす。
    (読めない環境では Lictor 経由トークン取得を代替に。)
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

> 実装メモ: `AnthropicSdkClient` に `getAuthToken`(OAuth)+ `enableCache`(system に cache_control)を追加。
> `readClaudeCodeToken` が `~/.claude/.credentials.json` を読む。フロー LLM 鎖 =
> worker-pool → **SDK(OAuth/cache)** → claude-p(FallbackLlm 入れ子, index.ts)。persona 発話は
> `buildPaperSystem`(安定=system) + `buildPersonaUserPrompt`(可変=user)に分割。
> ライブ確認済: サブスク OAuth で /v1/messages 成功・usage 返却 (~1s, claude-p の ~5s より速い)。
> 注意: cache は prefix が各モデルの最小トークン(Haiku 4.5=4096)以上で初めて効く。短いペーパーでは
> cache_creation=0 のまま (プラミングは正)。facilitator/vote/summary/conclusion は今は system 未使用
> (follow-up で system 化すれば更にキャッシュ範囲が広がる)。

## 実装順 (残)

1. ~~E (SDK/OAuth/caching)~~ ✅
2. ~~B 憑依配線~~ ✅ (テーマ類推)
3. G 壁打ち相手指定 — 未実装
4. F 合成 — 未実装
5. C 生成スクリプト (スタブ) — 未実装
