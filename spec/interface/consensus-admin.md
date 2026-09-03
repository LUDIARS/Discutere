# 管理用 Consensus / Gap API

## SPEC-THALEIA-PERSONA-CONSENSUS: GET /api/admin/consensus/:gapId/personas

管理者のみが利用できる。`discussion-of-gap:<gapId>` のペルソナ発話を、外部発話と facilitator を除外してペルソナ単位に集計する。

応答は private/no-store の `{ gapId, personas }`。各 persona は `personaId`、`displayName`、`traits`、時系列の `utterances`、発話の `opinionScore` 合計、`consensusScore` 合計、採点済み発話に対する `agreeRatio` を持つ。該当 session が無い場合は空配列を返す。

## SPEC-THALEIA-GAP-CREATION: POST /api/admin/gaps

管理者のみが利用できる。`Content-Type: application/json` の本文 `{ title, description, gameId? }` で design gap を起票する。既存の headless discussion seed を再利用して `discussion-of-gap:<gapId>` session と facilitator の開始発話も作成し、`{ gapId, sessionId }` を 201 で返す。`title` と `description` は空文字を受け付けない。`gameId` を指定する場合は、現在の workspace に存在する game の ID でなければならない。
