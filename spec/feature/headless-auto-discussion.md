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
| sources | ["genre"] | 種ソース ("genre" / "store-trend"、 複数で巡回) |

env: `DISCUTERE_AUTOSEED_ENABLED` / `_INTERVAL_MS` / `_MAX_CONCURRENT` / `_SOURCES`。

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
