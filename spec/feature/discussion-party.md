# 議論パーティ編成 + 続行/停止 + 半数入替 (DiscussionDirector)

`/debate` で起動するパーティ議論の編成・進行ルール。tick 固定ルール群とは別の
**DiscussionDirector** (`src/discussion/`) が議論単位で司る。設定は `config.discussion`。

## パーティ編成 (composition.ts)

- **総数** = `max(minTotal, ceil(想定発話数 / 4) + 1)`。既定 想定発話数=20 → **6 人**。
- **内訳**:
  - 司会 ×1 — `facilitatorModel` (既定 Opus)
  - キーマン ×`keymanCount` (既定 2) — `keymanModel` (既定 Opus)
  - 意見 ×(総数 − 1 − keymanCount) — `opinionModels` の重み抽選 (既定 Sonnet:Haiku=6:4)
- **GPT (codex) は不参加**: `workerPool.excludeProviders=["codex"]`。パーティは Claude 系のみ。
- **賛否はターン時に決定**: ペルソナに pro/con を固定せず、`decideStance()` が毎ターン
  キーマン/意見に賛成寄り or 否定寄りを 50:50 で振る。司会は neutral。
- 各メンバーは人間名 (名前プールから一意) + 役割別の口調/特徴を持つ。

## 進行 (director.ts)

1. パーティ編成。
2. ターンループ: 話者を巡回 (意見/キーマン、4 ターンに 1 回司会)、賛否を振り、直近発話
   を文脈に LLM 生成 → 投稿。空応答は skip (発話数に数えない)。
3. **想定発話数に到達**したら **続行/停止を問う** (Discord ボタン)。
4. **続ける** → 司会を除くメンバーの**半数を入替** (`swapHalf`) して次ラウンド。
   **止める / 無応答タイムアウト** → 司会が締めのまとめを投稿して終了。
5. `maxRounds` で暴走ガード。

### 半数入替 (swapHalf)

- 司会は不変。
- 入替対象 = キーマン + 意見。そのうち `floor(対象数 / 2)` 名をランダム選択して再生成。
- **キーマンはモデル維持** (Opus) で別人格、**意見はモデルも再抽選** (Sonnet/Haiku)。

## ワーカー (テスト時)

常駐 Lictor ワーカーは spawn せず、**claude -p** (`ClaudeCliClient`) でメンバーの model
を `--model` 指定して都度生成する。Opus/Sonnet/Haiku は全て claude なのでモデル指定が効く。

## Live 配線 (director-live.ts)

- **発話投稿**: `postDiscordWebhook` で persona の人間名で投稿 (フォーラムスレッド対応)。
- **続行/停止**: `[続ける] [止める]` ボタン (`debate:cont|stop:<token>`) を投稿し、
  button interaction で解決。`continueTimeoutMs` 無応答は停止扱い。
- **起動**: slash `/debate topic:<議題>`。gateway の InteractionCreate が
  `debateRunner.start(channelId, topic)` を非同期起動、button を `handleButton()` に委譲。

## 設定 (`config.discussion`)

| キー | 既定 | 説明 |
|------|------|------|
| `expectedUtterances` | 20 | 想定発話数 (人数算出 + 続行ゲート) |
| `keymanCount` | 2 | キーマン人数 (Opus 固定) |
| `facilitatorModel` / `keymanModel` | `claude-opus-4-8` | 司会 / キーマンのモデル |
| `opinionModels` | Sonnet:6 / Haiku:4 | 意見メンバーの重み抽選表 |
| `minTotal` | 4 | 総人数の下限 |
| `turnDelayMs` | 4000 | ターン間ディレイ (Discord ペース) |
| `continueTimeoutMs` | 120000 | 続行ボタン応答待ち (無応答は停止) |
| `maxRounds` | 5 | ラウンド上限 |

env override は `DISCUTERE_DISCUSSION_*` (EXPECTED / KEYMEN / MIN_TOTAL / TURN_DELAY_MS /
CONTINUE_TIMEOUT_MS / MAX_ROUNDS / FACILITATOR_MODEL / KEYMAN_MODEL)。

## 関連 / 今後

- [OVERVIEW](../OVERVIEW.md) / [persona-engine DESIGN](../persona-engine/DESIGN.md)
- 現状の起点は slash `/debate`。フォーラム新規ポスト (分類器 start_discussion) からの
  自動起動は今後の統合課題 (tick-debate との二重起動を避ける調整が必要)。
