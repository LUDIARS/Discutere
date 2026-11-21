# Headless 自動シード議論 (#63-66)

Discord 投稿を起点にしない「自走議論」。 ジャンルやストアトレンドを種に AI 同士が
議論し、 進行役 (facilitator) が止揚 (アウフヘーベン) 到達で締める。 生成された結論と
その裏の論述データを後から辿れる。

## 全体像

```
[種カタログ]                [シード]                  [駆動]                    [閲覧]
genres (17)  ─┐
              ├─→ seedHeadlessDiscussion ─→ facilitator (server常駐) ─→ 結論 ─→ /discutere-conclusions
store-trends ─┘   gap + session(scene=gap:*)   拡張→止揚判定→収束        (closed gap)   listConclusions
                  進行役が開幕で登場            進行役が【収束】で締める                 getConclusionDetail
```

## scene による headless 判定 (#63)

- `scene=discord:<guild>/<channel>` → Discord に投稿する通常議論。
- `scene=gap:<gapId>` → **headless**。 `discussion-bridge.postDiscussionToDiscord` は
  `{ ok:false, skipped:true }` を返し、 caller は warn を出さず黙ってスキップ (ログ noise 除去)。
- `facilitator.listActiveDiscussions` は `discord:*` と `gap:*` の両方を駆動対象にする
  (これが無いと headless 議論が拡張/収束されない)。

## シード (#64/#65)

- `discussion-seed/genres.ts` — ars-game-lexicon 準拠 17 ジャンルの静的カタログ
  (cross-repo path 依存は CI で壊れるため Di 内に持つ)。
- `discussion-seed/store-trends.ts` — Steam 公開 featuredcategories からトップセラーを取得
  (キー不要)。 失敗時は静的フォールバック。 App Store/Play は公開 API が薄く当面 Steam 中心。
- `discussion-seed/seed.ts` — discord 紐付けの無い gap + `discussion-of-gap` session
  (scene=`gap:*`) を立て、 進行役 (司会 結) が開幕の一言で「登場」する。
- `discussion-seed/scheduler.ts` — 定期的に種を 1 つ投入。 `maxConcurrent` で同時に開く
  headless 議論を抑制。 駆動は server 常駐の facilitator に委譲 (ここは種まきのみ)。

### 設定 (config.autoSeed)

| キー | 既定 | 説明 |
|---|---|---|
| enabled | false | 自動シードの有効化 (opt-in) |
| intervalMs | 1_800_000 | 種投入の周期 (30分) |
| maxConcurrent | 2 | 同時に開く headless 議論の上限 |
| sources | ["genre"] | 種ソース (`genre` / `store-trend` / `playtest-voices`、複数で巡回) |
| minNewVoices | 3 | `playtest-voices` で起動するゲームの未消費感想の最小件数 (整数、1〜10000) |
| cooldownMs | 86400000 | 同じゲームの再起動までの最短時間 (整数、0〜2592000000 ms) |

env: `DISCUTERE_AUTOSEED_ENABLED` / `_INTERVAL_MS` / `_MAX_CONCURRENT` / `_SOURCES` / `_MIN_NEW_VOICES` / `_COOLDOWN_MS`。

### 試遊感想による起動（Actio 3259cd8c）

`playtest-voices` は、Voluptas から受領した匿名の人の感想と AI 試遊感想を重複を除いて合算する。AI のゲーム ID を基準にまとめ、タイトルからゲーム ID を一意に引ける場合に人の感想を合算する。人の感想しか届いていないタイトルも起動候補になる。閾値に達し、対象の議論が開いておらず、クールダウンを過ぎたときに同じ headless 経路で起動する。起動成功時に対象の受信位置をワークスペース別に保存する。議論の種には人と AI の感想を別の欄に載せる。

既存ソースと試遊感想の切り替えは `native/src/discussion_seed/scheduler.cpp` が行う。

## CDGD 改善提案（SPEC-DI-PLAYTEST-PROPOSALS）

`playtest-voices` 由来の議論の収束時に、結論要約を永続 outbox に登録する。収束済みを確認してから Concordia の `POST /v1/management/events` に次の値を送る。

| 項目 | 値 |
|---|---|
| `event_key` | `di:proposal:<gap ID>`。同じ議論の再送で固定 |
| `source` / `kind` / `origin` | `di` / `improvement_proposal` / `ai` |
| `project_code` | 対象ゲームを管理するプロジェクトコード。初回配送時に確定 |
| `target_key` | Voluptas のゲーム ID。ID が無いときは `glab:<タイトルの SHA-256>`、Concordia のキー規則に合わない ID は `game:<ID の SHA-256>` |
| `summary` / `observed_at` | 結論要約（最大 2000 UTF-16 単位）と収束時刻 (UNIX ミリ秒) |

`managementProposals.baseUrl` に Concordia のループバック origin (127.0.0.1 または ::1)、`managementProposals.projectCode` に配送対象プロジェクトを設定する。どちらも既定は空。env は `DISCUTERE_MANAGEMENT_PROPOSALS_BASE_URL` / `DISCUTERE_MANAGEMENT_PROPOSALS_PROJECT_CODE`。認証情報は送信せず、リダイレクトを追わない。15 秒ごとに配送対象を確認し、5 秒のタイムアウトまたはエラー時は状態と警告を残して 60 秒後から同じキーで再送する。未設定時も保留理由を記録して再試行する。

## 契約の観測（SPEC-DI-PLAYTEST-RUNTIME）

C++ の受信形式、起動の閾値、配送イベントは `native/augur.contracts.json` に登録する。`native/src/playtest/contracts/contract_runtime.hpp` のランタイムは契約 ID と判定だけを記録する。後置条件を省略した場合も、戻り値のある処理と `void` 処理のどちらでも呼び出しを観測する。PR #4003（Actio `37ee6778-714c-4851-85dd-64da25da66a5`）の回帰確認では、両方を実体化する翻訳単位の Praesidium Clang 解析結果を確認する。

## 結論閲覧 (#66)

- `visualize/conclusions.ts` — `listConclusions` (収束済 gap 一覧) /
  `getConclusionDetail` (議論ログ・止揚ストック・高評価意見)。
- slash `/discutere-conclusions` — 引数なしで結論一覧、 `gap:<id>` で論述データ詳細。

## 手動実行 (CLI)

```
npm run headless                       # mock LLM / tmp DB / ジャンル種で 1 回
npm run headless -- --runs 3           # 3 回 (種を順に変える)
npm run headless -- --llm cli --genre ローグライク   # 実 LLM (claude CLI)
npm run headless -- --persist          # 本番 DB (data/) に書く
```

種→議論→収束を 1 プロセスで完結させる (server を立てずに検証できる)。

## 個人データ

匿名 workspace。 headless 議論は AI persona と進行役のみで、 編集者名・アカウント名を残さない
(CLAUDE.md / spec/feature/crawler/DESIGN.md 準拠)。
