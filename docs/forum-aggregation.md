# Discord フォーラムへの議論集約 (forum aggregation)

2026-06-06。Di の Discord 議論を **フォーラムチャンネル** に集約する。

## 目的 (ユーザ要件)

1. Di の Discord 議論を **すべてフォーラム** に集約する。フォーラムを「議論カテゴリ」
   として使う (例: 「ゲーム議論」フォーラム)。
2. **guild 内の Forum チャンネルすべて** を監視対象にする (個別設定不要)。
3. フォーラムの各ポストの **「最初の投稿」(starter message)** に対して議論をトリガーする。
4. 議論が **収束したらそのポストをクローズ** (archive + lock) する。
5. それ以外に、起動時に次のチャンネルを **自動作成** して各々処理する:
   - **データ学習依頼** チャンネル — 貼られた URL を外部データクロールに回す
     (既存 `crawlChannelIds` 機構を auto-create で起動)。
   - **まとめ投稿** チャンネル — 収束した議論の結論 (まとめ) を集約投稿する。

「議論の基礎学習用チャンネル」は要件から除外 (ユーザ判断)。

## 設計 — 既存パイプラインへの相乗り

議論ライフサイクルは既存実装をそのまま使う。フォーラムは「入口」を足すだけ:

```
[Forum 新規ポスト]
   ThreadCreate (parent=GuildForum)
      └ fetchStarterMessage() で最初の投稿を取得
         └ routeForumPost(isStarter=true)
              ├ ensureDiscordSession: scene = discord:<guild>/<threadId>
              ├ submitMessage: starter を utterance 化
              └ classifyInboundMessage (auto-discussion): designGap(open) を作成
                   evidence_json = { guildId, channelId=threadId, sessionId, ... }
   ↓ (既存) event-bridge が designGap を拾う
      └ ensureGapDiscussionSession: discussion-of-gap:<gapId>
           scene を evidence の source session = discord:<guild>/<threadId> に解決
   ↓ (既存) persona-engine + facilitator がスレッド内で自走議論
      └ AI 返信は discussion-bridge 経由で channelId=threadId に post = スレッドに出る
   ↓ (既存) facilitator が収束 → 【収束】post + closeGap(status=closed)
      └ ★追加: onConverged コールバック
           ├ スレッドを setLocked(true) + setArchived(true) = ポストをクローズ
           └ まとめを「まとめ投稿」チャンネルへ post
```

**鍵**: 議論 session の scene の channelId をスレッド id にすれば、AI 返信・収束まとめは
そのスレッドに出る。フォーラムは「scene の channelId = thread.id」を満たすだけで既存機構に乗る。

## 入口の挙動

| イベント | 条件 | 処理 |
|---|---|---|
| `ThreadCreate` | 親が `GuildForum` | starter を取得し議論を起動 (一度きり / 既存 open 議論があれば skip) |
| `MessageCreate` | forum スレッド内 & `msg.id === channelId` (=starter) | skip (ThreadCreate が処理済) |
| `MessageCreate` | forum スレッド内 & 上記以外 (=返信) | utterance ingest のみ。新規 gap は立てない → event-bridge が人間発言をミラー (最優先即応) |

- **starter = フォーラムポストの最初の投稿**: discord ではフォーラムポストの開始メッセージ id は
  スレッド id と一致する。返信判定に `msg.id !== msg.channelId` を使う。
- 起動は `ThreadCreate` を正トリガにする (フォーラムポスト新規作成の確実なシグナル)。
  `fetchStarterMessage` は作成直後に取りこぼすことがあるため 1 回リトライする。

## 収束 → クローズ (onConverged)

- facilitator に optional `onConverged({ gapId, sessionId, scene, summary, title })` を追加。
  `converge()` の closeGap 直後に呼ぶ (純粋にデータを渡すだけ。discord 依存は持たせない)。
- index.ts が facilitator 生成時に late-bound ブリッジを渡し、Gateway 起動後に
  discord client を握る finalizer を結線する。
- finalizer (gateway 側、discord client を持つ):
  - scene が `discord:<guild>/<threadId>` かつ thread の親が `GuildForum` のときだけ作動。
  - `setLocked(true)` → `setArchived(true)` でポストをクローズ。
  - 「まとめ投稿」チャンネルへ `formatForumSummary(title, summary)` を post。
  - 通常チャンネル (非フォーラム) の収束では何もしない (従来通り)。

## 自動作成チャンネル (ClientReady, guild ごと)

`ensureSystemChannel` (既存) を再利用して 2 チャンネルを ensure する:
- **データ学習依頼**: 作成後、id を runtime の crawl 集合に追加 → 既存 `handleCrawlMessage` が処理。
  結果通知は従来通り「データ追加」チャンネル。
- **まとめ投稿**: 収束まとめの集約先。finalizer が都度 ensure (cache) して post。

権限 (Manage Channels) が無い guild では作成を skip し、既存同名チャンネルがあれば流用する
(`ensureSystemChannel` の既存挙動)。

## config 追加 (`discord.forum`)

```jsonc
"discord": {
  "forum": {
    "enabled": true,                  // フォーラム監視の on/off (既定 true)
    "summaryChannelName": "まとめ投稿",
    "dataLearningChannelName": "データ学習依頼",
    "managedCategoryName": "システム"
  }
}
```

env: `DISCUTERE_DISCORD_FORUM_ENABLED` 等。既定で有効 (guild に Forum を作れば即監視)。

## 既存 `discussionChannelIds` の扱い

破壊しない。フォーラムが議論の正路となるが、平文議論チャンネル機構は後方互換で残す
(既定で空 = 無効)。「集約」はフォーラムを正面に据えることで達成し、設定済みチャンネルは
そのまま使える。

## 変更ファイル

- 新規 `src/discord-hook/forum-monitor.ts` — forum 判定 / starter 判定 / finalizer (archive+lock+summary)。
- 新規 `src/discord-hook/managed-channels.ts` — ClientReady で データ学習依頼 / まとめ投稿 を ensure。
- 改 `src/discord-hook/command-router.ts` — `routeForumPost` (ingest + 条件付き classify、channel gate 無し)。
- 改 `src/discord-hook/system-channel.ts` — `topic` option を追加 (用途別 topic)。
- 改 `src/discord-hook/gateway.ts` — ThreadCreate / forum 返信 / 自動作成 / finalizer 公開。
- 改 `src/persona-engine/facilitator/facilitator.ts` — `onConverged` コールバック。
- 改 `src/index.ts` — onConverged ↔ gateway finalizer のブリッジ結線。
- 改 `src/config.ts` — `discord.forum`。
- テスト: forum 判定 / starter 判定 / summary 整形 (純粋関数)。
```
