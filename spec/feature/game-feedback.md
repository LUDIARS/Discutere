# ゲーム感想チャンネル (匿名意見収集)

Discord に「ゲーム感想を投稿するだけ」のチャンネル群を用意し、投稿を議論にせず
**意見データとして匿名収集**する。集めた感想は議論を実際の人間の声に接地させる素材
(将来 classifier / persona プロンプトに差し込む)。

## 仕組み

- **カテゴリ「ゲーム感想」** (`config.discord.gameFeedback.categoryName`、既定「ゲーム感想」)
  を起動時に ensure。無ければ作成する (Manage Channels 権限が要る)。
- その**カテゴリ配下のチャンネル (チャンネル名 = ゲームタイトル)** はユーザが作って投稿する。
- カテゴリ配下のテキスト/announcement チャンネルへの投稿は、議論ルーティングに回さず
  **感想として収集** (`extractGameFeedback` → `onGameFeedback` → `GameFeedbackStore`)。
  収集できた投稿には 📝 リアクションを付ける。
- 収集後は議論を起こさない (`return`)。フォーラム/平文議論経路とは独立。

## 保存 (匿名)

`GameFeedbackStore` (`src/feedback/store.ts`、専用 `data/game-feedback.db`):

| 列 | 内容 |
|----|------|
| `game_title` | チャンネル名 (= ゲームタイトル) |
| `content` | 投稿本文 |
| `created_at` | 投稿時刻 |

**投稿者名・アカウント名は保存しない** (個人データ規約 / CLAUDE.md「議論ノードに編集者名を
保存しない」)。API: `add` / `recentForGame` / `countForGame` / `listGames`。

> 注意: Discord はチャンネル名を正規化する (小文字・ハイフン化) ため、`gameTitle` は
> 正規化後 slug ("vampire-survivors" 等)。classifier の `gameTitle` と突合するときは
> 正規化が必要 (今後の課題)。

## 設定 (`config.discord.gameFeedback`)

| キー | 既定 | env |
|------|------|-----|
| `enabled` | true | `DISCUTERE_DISCORD_GAME_FEEDBACK_ENABLED` |
| `categoryName` | ゲーム感想 | `DISCUTERE_DISCORD_GAME_FEEDBACK_CATEGORY` |

## 今後 (item 2 連携)

収集した感想 + 過去議論を、そのゲームの議論を起こす/進めるときに classifier / persona の
プロンプトへ「実際の人間の感想」として差し込む (議論を実データに接地、人間意見を無視しない)。
本 PR は収集基盤まで。差し込みは別タスク。→ [OVERVIEW](../OVERVIEW.md)
