# T7: 起動経路 (簡素 WebUI + Discord/Slack トリガ + フロー選択)

設計参照: `spec/flow/OVERVIEW.md` §9 (起動経路) / `spec/flow/discussion.md` step 1。依存: **T2**
(T3-T6 完了後が望ましいが、入口だけなら T2 後に着手可)。

## ゴール

4 フローを**最初の投稿**で起動する正式な入口を 2 経路で用意する。テーマ + タグ + フロー選択を
受けて FlowDirector を起動する。

## スコープ

### in
1. **経路 A: 簡素 WebUI** (`src/flow/web/` + ルート):
   - loopback の 1 ページ。テーマ入力 + **議論タイプ (フロー) の必須選択** + タグ選択 + 送信。
   - 既存 Web チャット (`spec/feature/web-chat.md`, `scene=web:<room>`) のトランスポートを流用。
   - 進行状況・発話・結論を表示 (既存ポーリング機構を流用)。

2. **経路 B: Discord フォーラム投稿を UI とする** (`src/flow/entry-discord.ts` 等):
   - **フォーラムの投稿が UI**。最初の投稿 (starter) をトリガーにテーマ受信。既存 Gateway /
     フォーラム starter (`forum-monitor` / `command-router`) を流用。
   - **議論タイプ (フロー) とタグ (機密/内部/運用/開発) は、いずれもフォーラムの適用タグから取得**
     (slash / ボタンは使わない)。議論タイプは必須。
   - 認証は既存どおり Discord 依存 (bot token + admin allowlist、Cernere 非使用、`CLAUDE.md`)。
   - Slack 経路は後続 (初版は Discord 先行)。

3. **共通ディスパッチ** (`src/flow/dispatch.ts`):
   - `(theme, tags, flow, scene)` を受け、対応する FlowDirector / 学習 / 壁打ちを起動する単一入口。
   - **議論タイプ (flow) で分岐**。flow=壁打ち の時のみ §sparring の進行に入る (壁打ちは投稿時の
     議論タイプ選択で判断する。発話内容での自動判定はしない)。
   - 議論タイプ未指定の投稿は受理しない (必須)。
   - T2 で作った dev 用トリガはこの正式入口に置換する。

### out
- 各フローのロジック (T2-T6)。新しい認証層 (既存 Discord 依存を流用)。

## 受け入れ条件 (テスト)
- WebUI からテーマ + タグ + フロー指定で議論が起動し、発話と結論が表示される。
- Discord 投稿起点で同じく起動する (gateway は mock で可)。
- フロー選択に応じて正しいドライバ (議論/改善/学習/壁打ち) が呼ばれる。
- タグが投稿/UI から拾われ、外部収集可否 (機密/内部) に反映される。

## 関連
- `spec/feature/web-chat.md` / `src/web-chat/` / `src/api/web-chat-routes.ts` /
  `src/discord-hook/{gateway.ts,forum-monitor.ts,command-router.ts}` / `CLAUDE.md` (認証)。

## 決定事項
- Discord はフォーラム投稿を UI とし、議論タイプ + タグは**フォーラム適用タグ**から取得 (slash/
  ボタン不使用)。Slack は後続、初版は Discord 先行。議論タイプは必須選択。壁打ちもこの選択で判断。
