---
task: actio-discussion-participation
project: Di
kind: feature
created: 2026-09-27
memory_links: []
---

2026-09-27 neco「OKそれで実装はじめて」を根拠に、承認済みのDi/Actio/Cc分担を実装。

DI-EXTERNAL-DISCUSSION: Actioの確認スレッド/スプリントチャンネルの会話を受け、約3分の無返信や活発な会話で任意参加を判断し、賛否の観点を持つ短い発言案を返す。場所ごとの同意と配送はActioが所有。Diから直接投稿しない。通常会話によるタスク作成/承認は行わない。

既存LLM接続とdecideStanceを再利用。機会ごとの抽選・結果保存、scene排他、リース所有権、同じ人間投稿への再応答抑止、認証、入力/同時実行上限を追加。外部会話にツール権限を与えず、未対応の会話専用経路はエラーにする。

関連: Actio `feat/sprint-chat-integration`、Cc `feat/actio-chat-transport`。秘密と接続先を設定し、対応版を配備するまでは有効化されない。

静的型確認は通過。tests/flow/external-discussion.test.tsを追加・runnerへ登録。人間の明示許可がないため、テスト実行・起動・実投稿・マージは未実施。
