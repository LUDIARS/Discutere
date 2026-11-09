# Di管理フォーラムの受信境界

DiのDiscord利用者イベントは、設定された各guildで `discord.forum.discussionForumName` から
`ensureDiscussionForum` が解決したフォーラムIDを親とするスレッドだけで処理する。
フォーラム名の一致や `GuildForum` 型だけでは許可しない。guild IDと親forum IDの組を照合する。
guildごとに既存フォーラムの名前が違う場合は `discord.forum.discussionForumNameByGuild` (guild ID→名前、C++版のみ) で
そのguildの解決に使う名前を指定できる。空や未指定のguildは `discussionForumName` を使う。指定しても解決後の照合は同じ。
起動時の解決は `forum.enabled` に従い、`flowLive` の有無とは分ける。LLM無しでもslashの境界は同じ。
ClientReadyで対象IDが解決する前、未設定・無効・未解決のguild、親の取得失敗は拒否する。
親がuncachedなら読み取りfetchでID/guild/typeを確認する。複数guildの対象を混同しない。

Botが受信できなかった既存スレッドは、管理者の `POST /api/admin/forum/threads/:id/start` で新規投稿と同じThreadCreate経路へ渡す (C++版のみ)。
同じgateを通すので、管理フォーラム外のスレッドは起動しない。

MessageCreateは共通gateを最初に通し、ゲーム感想、クロール、旧discussionChannelIds、メンションが
通常テキストチャンネル・DM・他フォーラムを許可する迂回路にならないようにする。
Discordのreply投稿そのものは、宛先にかかわらず引き続き無視する。
ThreadCreate/starter、承認・スコアリングのreaction、button/modal/select/slashの全interactionも同じgateを通す。
対象外では取り込み・callback・返信・メニュー・リアクション・状態書込を行わない。
明らかな対象外guild/parent/typeはfetch前に拒否し、未解決の対象候補だけ読み取り解決する。

componentに埋め込まれたthread/channel IDは実interaction.channelIdと一致させる。
別スレッドのpending設定やペーパーレビューを操作しない。debate継続tokenも生成元channelに結び付ける。
管理フォーラム内の既存フロー、ペーパーレビュー、再議論、one-shot診断、adminの処理は維持する。

seed sweepは旧テキスト監視集合を走査せず、解決済みguildの管理フォーラムスレッドだけを対象にする。
メッセージ取得・取り込み前にも同じ判定を行う。公開reactToMessageとfinalizeForumPostは、対象スレッドを
共通scopeで確認してからメッセージ取得/リアクション/lock/archiveへ進む。
内部onConcluded/onReproposeFlowTypeにも判定を適用し、対象外ではpending更新も出力もしない。
明示されたまとめ・monitor等の運用出力先の撤去や、汎用posterの変更はこの境界修正に含めない。

純粋ID判定と読み取り解決は `managed-forum-scope.ts`、実gatewayのイベント登録gateは
`forum-event-gate.ts` に分ける。回帰fixtureは同じ登録gateを使い、対象外のcallback/API書込がゼロ、
管理スレッドでのみ既存callbackに進むこと、reply無視、uncached親、ID不一致を検証する。
手元の検証は静的型検査・差分検査だけ。テストとサービスの実行は行わない。
