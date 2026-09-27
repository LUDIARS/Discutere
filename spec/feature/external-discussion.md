---
id: DI-EXTERNAL-DISCUSSION
type: feature
title: Actioの会話への任意参加
---

2026-09-27 neco「OKそれで実装はじめて」で承認。

議論場所はActioが管理する通常チャンネルと確認・議題スレッド。Diは参加判断と発言案のみを返し、チャンネル作成・Discord投稿・タスク登録・承認は行わない。Ccの有無はDiに影響しない。

`POST /internal/external-discussion/consider` は配備で注入する `DISCUTERE_EXTERNAL_DISCUSSION_SECRET` のBearer認証が必須。未設定・LLMなしは503。要求は `scene: actio:<opaque scope>`、`enabled`、最大80件/32000文字の `messages: [{id,kind:human|ai,text,at:UTC epoch milliseconds}]`。個人の表示名・アカウント情報は送らない。

有効時のみ、最後の人間の投稿から180秒の無返信、または180秒内に人間の投稿が3件以上ある機会を評価。静かな時65%、会話中15%でLLMに参加の妥当性を判断させる。抽選結果は機会ごとに保存し、ポーリング回数では確率を上げない。AI発言だけで再発火しない。新しい観点がなければ発言しない。賛否は既存 `decideStance` を再利用する。

結果は `disabled|waiting|busy|skipped|proposal`。proposal時は `proposalId,text,stance,sourceMessageId`。同一機会の再取得は同じ結果。SQLiteで排他・リース・所有権を管理し、180秒の連投抑止と同じ人間投稿への再応答抑止を行う。呼出側はproposalIdを送信箱の重複防止キーとし、送信直前に有効設定と新着の人間投稿を再確認する。

会話本文は推論要求に限り使用し、DiのKGに複製しない。保存するのは対象scene・機会ID・人間投稿ID・発言案・処理状態。既存の議論フローのDB接続とLLM接続を利用する。既存フォーラム運用は変更しない。

検証用ケースは `tests/flow/external-discussion.test.ts`。明示許可がないため作成のみで未実行。実際の投稿・配備設定変更・再起動も未実施。

外部会話はconversationOnlyで推論し、Claude CLIはtools/MCP/skillsを無効化する。常駐ワーカーはこの要求を拒否し、既存の推論専用チェーンへ戻す。Codex CLIの会話専用capabilityは未対応として明示エラーにする。API/local推論、またはツールを無効化したClaude経路を設定する。通常の議論フローの権限は変更しない。
