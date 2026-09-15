# コメント履歴 → Vo → Di

仕様 ID: SPEC-DI-COMMENT-HISTORY-001。Vo側は SPEC-VO-COMMENT-HISTORY-001。

## 合意した範囲

YouTube の動画コメントとライブチャット、Steam公開レビューから、投稿者単位の
観測履歴を作る。動画コメントとSteamは過去6暦月（UTC、月末は有効日に丸める）、
ライブチャットは最初の収集開始以降。ニュース限定ではない。
YouTube内は公開channel IDの一致で統合し、SteamIDとは人物照合しない。
対象動画・ゲームで観測した範囲であり、その人の全発言履歴を取得したとは扱わない。

## 収集と再開

`comment-history.example.json` をコピーし `videos` と `steam` を設定する。
videosの要素は `{ "id": "11文字の動画ID", "topic": "話題slug", "comments": true, "livechat": true }`、
steamの要素は `{ "appId": 123, "topic": "ゲームslug" }`。
空の対象リストは失敗する。topicはゲーム以外の話題にも使える。

```sh
npm run comment-history -- collect <config.json>
npm run comment-history -- export <config.json> <private-snapshot.json>
```

collect は `maxRequests` 以内の1巡で終了する。定期実行のたびに保存済みcursorから再開する。
ライブ継続には定期実行が必要で、常駐サービスを自動起動しない。
`pollingIntervalMillis` より早い問い合わせをしない。停止中の取りこぼしは復元保証がない。
動画IDから activeLiveChatId を公式APIで取得する。終了した配信の履歴を遡るための
yt-dlpや非公式APIへの切替はしない。

動画コメントは `commentThreads.list`、全返信は `comments.list` で別途ページングする。
親が古くても新しい返信を持つため、古い親で走査を止めない。Steamはrecent順で境界まで辿る。
日次再走査で更新を確認し、見つからなくなった古いデータも期限で除去する。
source/nativeIdでupsertし、同一投稿の重複蓄積を避ける。
匿名IDのunknownを一人にまとめない。

## 予算・保存・利用条件

YouTube API key は `YOUTUBE_API_KEY` のみ。URL・応答本文・キーはログへ出さない。
予約した日次予算をSQLiteへ送信前に記録する。失敗・クラッシュで消費予約は戻さない。
太平洋時間の日付でリセットし、他の既存クローラとの合計が無料枠を超えないよう
Cloud Consoleで専用サブ予算を確保する。同じプロジェクト全体の利用量を自動取得した値ではない。
ライブを使う場合 `liveChatRequestUnits` に対象プロジェクトで確認した実効コストを明示する。
search APIは使用せず、対象動画IDを直接指定する。

YouTubeデータは既定30日以内に再取得または削除。期間指定と保存期限は別物。
長期ライブ履歴の保持はAPIの追加承認なしには実現しない。
承認済みの場合のみ `youtubeRetentionDays`（最大186）と `youtubeApprovalReference` を設定する。
文字列は実際の承認の代わりにならず、YouTube派生データ利用の承認はVo側でも別途検証する運用。
承認未確認のYouTubeからペルソナ形成を実行しない。

DBはcollect/export時に期限切れを削除する。停止期間中の削除は運用者の定期実行が必要。
snapshotは内部資料で公開IDと本文を含むため、公開・リポジトリcommitしない。
出力は上書き禁止、expiresAt（最大30日）までに運用者が削除する。
同じDBへの複数collectorを短期leaseで排他し、通信タイムアウトより長いleaseを各ページ前に更新する。

## Vo形成とDi取込

Voの `player-profile-server` で次を使う（Vo側変更を先に配備する必要がある）。

```sh
npm run import:comment-history -- <private-snapshot.json> <personas.jsonl> 10
```

Diへ戻す:

```sh
npm run persona:import -- --input <personas.jsonl>
```

既存ブリッジv2に `historyEvidence` を追加し、固定20次元・有限値・スコア域と
証拠の期間・件数・有効期限を検証する。ゲーム評価に言及しない投稿は既存中立値となり、
その人の嗜好が判明した意味ではない。部分観測として表示し、原文や元IDはDiペルソナ出力に含めない。
期限切れの外部プロフィールはプールアクセス時に削除し、再取込で新しい証拠へ更新する。
永久稼働しない環境のデータ削除はDBファイル/バックアップの運用も別途必要。

## 受入条件

- 6暦月の月末境界、返信の独立ページング、Steam期間終端を扱う。
- YouTube共通IDを照合し、unknownや他サービスのIDを統合しない。
- quotaはエラー・プロセス再実行後も残り、ライブの待機時間を保存する。
- 未取得、途中、API拒否、ライブ開始前を完了と見せない。
- Voの20D出力をDiが読み、期限切れは拒否・プールから除去する。

公式資料（2026-09-15確認）:
- https://developers.google.com/youtube/v3/live/docs/liveChatMessages/list
- https://developers.google.com/youtube/v3/docs/comments/list
- https://developers.google.com/youtube/v3/determine_quota_cost
- https://developers.google.com/youtube/terms/developer-policies
- https://partner.steamgames.com/doc/store/getreviews
