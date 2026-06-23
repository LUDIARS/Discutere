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

## C. ペルソナ生成 — 確定設計 (2 系統。2026-06-15 レビュー反映)

実在ユーザ採用 (C1) と 合成生成エンジン (C2) の 2 系統。手動投入 CLI
(`npm run persona:generate`) は当面のシード手段として残す (`scripts/persona-generate.ts`)。

### C1. 実在ユーザ採用 (クロール由来) — まず実装

YouTube/note/Steam 等。話者アンカー `ext:<source>:<authorId>` (`sources/persona.ts toSpeakerId`)
で意見を集約し、**意思ある人 (ゲーム嗜好あり)** をペルソナ採用する。

**採用条件 (全て満たす)**:
- 意見 **全ゲーム横断で ≥10**。
- **全ネガ / 全ポジを除外** (polarity が片寄りきっている人は弾く。pos==total or neg==total を除外)。
- **ゲーム間でベクトル差分 (gap) が必ずある** (per-game affect が全ゲームで同一でない = ゲームを区別している)。
  → 実質 **≥2 ゲーム** + per-game ベクトルの最大対距離 > ε。

**affect 計算** (#125 非 embedding 改善後): 短文の中立丸まりを抑えるため **ゲーム単位で本文を連結してから
1 回ベクトル化** (粒度) し、per-game ベクトルを **opinion-score (= いいね数 +1) で重み付け平均**する
(本人の偏り + エンゲージメントを保持)。加えて **母集団平均からの距離 (典型度 typicality)**、
**極性偏り (polarity_bias)**、**ゲーム間ばらつき (affect_dispersion)** を保持し、中立に潰れたベクトル
同士の分離を補う。口調 (speechStyle) は**重要でないので付けない** (LLM 不要 = 安価・データのみ)。

**保存**: `flow_persona` に `origin="adopted"` + `source_speaker_id` (= `ext:source:authorId`) で **upsert**
(再クロールで意見が増えても別個体を量産しない)。露出名は `論者#xxxxxx` (`maskedPersonaLabel`、個人データ方針)。

**起動**: バッチ後処理 `npm run persona:adopt -- [--source youtube] [--min-opinions 10] [--dry]`
(crawl 完了後に KG を集約 → 条件フィルタ → 採用)。crawl runner インラインではなく後処理バッチ。

**取り込み (YouTube)**: `npm run import:youtube -- --input <jsonl> --game-mode video`。
`--game-mode video` は gameSlug を動画単位 (`yt:<videoId>`) にする → 「複数動画で反応が違う人」が
gap 条件を満たし採用対象になる (単一 game だと全員 no-game-gap で却下)。
実績 (2026-06-15 モンスト×ナルト 14万コメント): youtube 話者 67,732 → **採用 1,837**
(却下: too-few 65,732 / no-game-gap 162 / all-positive 1)。
**所見**: 短いコメントを 20 次元 lexicon でベクトル化すると中立寄りに丸まり、typicality が
~0.99 に集中して解像度が低い (avg 0.993, min 0.844)。
**#125 非 embedding 改善後の実測** (2026-06-16, 全 source 335,197 話者再集約 → 採用 1,836):
ゲーム単位連結 + opinion-score 重み付けでも **cosine-typicality は依然潰れる** (avg 0.9931 / std 0.0121)
— lexicon の presence-count では連結が共通次元を増やし cosine 方向がむしろ似通うため (構造的限界、
方向分離は embedding が必要)。一方で **新特徴量が良く分散**し分離材料になった:
polarity_bias (avg 0.152 / std 0.153 / 0–0.93)、affect_dispersion (avg 0.458 / std 0.270 / 0.02–2.27)。
→ 「平均値グループ判定」は typicality 単独でなく **(polarity_bias, affect_dispersion) を併用**するのが正。
残改善余地 = embedding ベース affect (cosine 方向の分離) / これら特徴量の 憑依・クラスタリングへの組込み。

### C2. ペルソナ生成エンジン (合成) — C1 の後

アンケート形式の行動基準 (嗜好 / プレイスタイル / 感情の波 / 年代 / 課金額) を構造化スキーマ化 →
合成個体 = 1 組の回答。ランダム抽選ゲームを「行動基準で“プレイしたか”確率判定」→ プレイ済のみ LLM で
感想生成 (未プレイは「やってない」と明示) → affect 化。

**母数判断 = 案A (実データ突合)**: 合成個体の意見クラスタを **C1 の実クロール分布と突合**し、
「この嗜好クラスタは現実で母数が大きい/小さい」を推定する (合成数そのものは母数でない)。
**身内データは 1 サンプル**として実分布に混ぜる。クラスタリングは affect 20 次元の cosine。
コスト大 (合成数 × 抽選ゲーム × LLM) → バッチ + キャッシュ前提。

### 実装フェーズ
1. **C1-a** ✅: `persona-adopt.ts` (`evaluateSpeakers`/`adoptPersonas`) + `npm run persona:adopt`
   (KG 著者集約→条件フィルタ→affect/typicality→`source_speaker_id` upsert/origin=adopted/`論者#`)。
   migration flow_0006 (source_speaker_id/typicality)。affect 解像度向上 (#125, 非 embedding) 済:
   ゲーム単位連結ベクトル化 + opinion-score (いいね) 重み付け + polarity_bias/affect_dispersion
   特徴量追加 (migration flow_0008、weight は `persona-adopt-runner` が reactions から算出)。テスト済。
2. **C1-b** ✅: crawl runner 自動採用フック。`flow.autoAdoptOnCrawl` (env `DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL`,
   既定 false) が有効なら `ext-ingest`/`ext-import` 完了後に C1 採用を自動実行
   (`src/crawler/sources/cli.ts autoAdoptAfterCrawl` → `src/flow/persona-adopt-runner.ts runAdoptFromKg`、
   ingest は直近 source のみ filter、採用失敗は crawl 成否に影響させない)。手動バッチ (`persona:adopt`) と
   集約/採用ロジックを共有する。
3. **C2-a** ✅: `survey.ts`(アンケート: 嗜好/プレイスタイル/感情の波/年代/課金額 + playProbability)
   + `persona-survey.ts`(`generateSyntheticPersonas`: 合成個体→プレイ判定→LLM感想→affect平均, origin=generated,
   learningSource=survey, 未プレイは明示)。`npm run persona:survey -- --count N`。
4. **C2-b** ✅: `persona-populations.ts`(`estimatePopulations`: 合成を貪欲クラスタ→各クラスタの
   **実分布(adopted)近傍数で母数大小**判定, realOrigins で身内sample混在可)。`npm run persona:populations`。
   テスト済。母数推定値 (判定 大/小 + 実近傍比率 + 推定時刻) は `persistPopulations` で各合成ペルソナの
   `population_verdict`/`population_ratio`/`population_estimated_at` (migration flow_0007) に書き戻す
   (CLI 既定で保存、`--no-persist` で抑止)。

> プールが空なら憑依 (B) / 壁打ち相手 (G) は従来生成にフォールバック。

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

## F. ペルソナ合成 — ✅ 実装済み

- `src/flow/persona-synthesize.ts`: `synthesizePersona({parentRefs, llm, weights?, label?})`。
  - affect = `averageVectors` (親ベクトルの重み付き平均)。
  - name/口調/traits = LLM で融合 (JSON、失敗時は機械フォールバック: name=連結 / traits=和集合)。
  - `insertPoolPersona({origin:"synthesized", parentIds})` で保存 → 憑依/壁打ち相手/通常議論で使える。
- 起動: `npm run persona:synthesize -- --parents "親1,親2" [--label ...]` (LLM=SDK OAuth→claude-p)。

## G. 壁打ち相手のペルソナ指定 — ✅ 実装済み

- `SparringSession.deps.opponentPersonaIds` (id/name)。`findPoolPersona` で解決 → 相手に充てる。未解決/未指定は従来生成。
- 経路: WebUI = テキスト入力「ペルソナ名/ID (カンマ区切り)」/ Discord = starter 本文 `相手:<名前>` (`parseOpponents`)。
- dispatch `DispatchInput.opponentPersonaIds` → SparringSession に注入。

> 実装メモ: `AnthropicSdkClient` に `getAuthToken`(OAuth)+ `enableCache`(system に cache_control)を追加。
> `readClaudeCodeToken` が `~/.claude/.credentials.json` を読む。フロー LLM 鎖 =
> worker-pool → **SDK(OAuth/cache)** → claude-p(FallbackLlm 入れ子, index.ts)。persona 発話は
> `buildPaperSystem`(安定=system) + `buildPersonaUserPrompt`(可変=user)に分割。
> ライブ確認済: サブスク OAuth で /v1/messages 成功・usage 返却 (~1s, claude-p の ~5s より速い)。
> 注意: cache は prefix が各モデルの最小トークン(Haiku 4.5=4096)以上で初めて効く。短いペーパーでは
> cache_creation=0 のまま (プラミングは正)。facilitator/vote/summary/conclusion は今は system 未使用
> (follow-up で system 化すれば更にキャッシュ範囲が広がる)。

## 実装状況

A ✅ / D ✅ / 共通基盤 ✅ / B ✅(テーマ類推) / E ✅(SDK OAuth+cache) / G ✅ / F ✅ /
C ✅(C1-a/C1-b/C2-a/C2-b 全実装。affect 解像度向上は Issue #125 で別途)
