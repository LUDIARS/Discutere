# 軽量 Web チャット議論 (web-chat)

Discord を介さず、 ブラウザの軽量チャット UI から Di の議論に参加する経路。
Discord-only pivot 後に「Discord 以外でも議論したい」要望に応える最小実装で、
**既存の議論エンジン (分類器 → designGap → persona-engine → facilitator) を
そのまま再利用**する。 違いは「トランスポート」だけ。

## トランスポート抽象 = `scene`

Di の議論は session の `scene` 文字列でトランスポートを束ねる。

| トランスポート | scene 形式               | 出力先                         |
| -------------- | ------------------------ | ------------------------------ |
| Discord        | `discord:<guild>/<chan>` | webhook で Discord channel へ  |
| **Web チャット** | `web:<roomId>`           | 投稿せず core に永続 → ブラウザがポーリング取得 |

`event-bridge` の 2 つの scene 判定 (議論 session への scene 継承 / 入口人間発話の
議論へのミラー) を `discord:` 限定から `isBoundScene()` = `discord:` または `web:` に
一般化した。 これにより web room も Discord と等価に「人間優先即応」「収束まとめ」まで
回る。 出力側 (`discussion-bridge`) は `discord:` 以外を `skipped` で黙って無視するので、
web scene の persona 発話は Discord に漏れず core にだけ残る。

## データ構造 (1 room = 1 scene)

1 つの room (`scene=web:<roomId>`) に 2 種の session がぶら下がる:

- `web-session:<roomId>` … 人間の平文発話 (入口)
- `discussion-of-gap:<gapId>` … persona / facilitator の発話 (+ ミラーされた人間発話)

`transcript.readRoomTranscript` は scene で両 session を束ねて 1 本の会話にする。
人間発話は入口・議論の両方に載る (ミラー) ため、 **人間は入口 session 由来のみ /
非人間は議論 session 由来のみ** を採用して二重表示を防ぐ。 個人アンカーは保存せず
(匿名 workspace)、 author は表示名のみ。

## エンドポイント

| メソッド | パス                          | 役割                                                   |
| -------- | ----------------------------- | ------------------------------------------------------ |
| GET      | `/chat`                       | チャット UI (1 ページ HTML、 依存ゼロ、 loopback 前提) |
| GET      | `/api/chat/:room/messages`    | 会話取得 (`?since=<ms>` で増分ポーリング)              |
| POST     | `/api/chat/:room/messages`    | 人間発話投入 `{text, author?}`                         |

認証は他の HTTP UI と同じく **loopback 信頼** (`middleware/auth`)。 公開はしない。

## 議論の起こし方

POST は入口 session に人間発話を記録し、 **その room でまだ議論が立っていなければ**
gateway と同じ分類器 starter (`createDiscordAutoDiscussionStarter`) を呼んで designGap を
起こす (`guildId="web"`, `channelId=<room>`, `forumTitle=本文`)。 既に議論中なら起こさず、
`event-bridge` が人間発話を進行中議論にミラーして即応させる。 Web チャットは本文を主題
アンカーにするので、 ゲーム名が特定できなくても議論が始まる (Discord 平文経路より緩い)。

## 実装

- `src/web-chat/session.ts` — `webScene` / `ensureWebSession` / `webDiscussionExists` / `sanitizeRoomId`
- `src/web-chat/transcript.ts` — `readRoomTranscript` (scene 束ね + 重複除去 + 役割/表示名)
- `src/api/web-chat-routes.ts` — HTTP ルート + 埋め込み HTML (`setWebChatDeps` で DI)
- `src/discatier-engine-adapter/event-bridge.ts` — `isBoundScene` 一般化 (web を Discord と等価に)
- `src/index.ts` — `/chat` マウント + 分類器 starter / persona 表示名 resolver を注入
- テスト: `tests/web-chat/run.ts` (`npm run test:web-chat`)

## 既知の制約 (プロトタイプ段階 / 粗く動かす)

- LLM backend (`llm.backend`) が設定されていないと persona は応答しない (Discord と同条件)。
- ポーリングは 2.5 秒間隔の HTTP (SSE/WebSocket は未導入)。
- 1 room に複数議題が同時進行すると同一チャットに混在表示される (room を分けて運用)。
- マルチユーザの同時編集・presence は無し (個人ローカル用途想定)。
