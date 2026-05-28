# REVIEW_MISSING_FEATURES — Discutere (2026-05-29)

評価: **C**

## 根本的未完成機能

| 機能 | 状態 | 説明 | 優先度 |
|------|------|------|--------|
| Slack webhook signature 検証 | ❌ X-Slack-Signature HMAC check 未実装 | src/machina/routes.ts:750。 Slack bot signing secret は monitor に保存されているが未使用 | 最高 |
| Discord webhook signature 検証 | ⚠️ Ed25519 関数実装済、 未使用 | src/discord-hook/interactions.ts:3 で verifyDiscordInteraction() 実装完了。 routes.ts:781 で未呼び出し | 最高 |
| Discord Interactions endpoint | ⚠️ Scaffold only | interactionToCommand() あるが /api/discord/interactions route 不在 | 高 |
| Bot Gateway 常時接続 | ❌ 未実装 | @discordjs/ws Gateway 接続 (Di-1 scope) | 高 |
| checkGroupAccess 実際の実装 | ❌ Stub のみ | src/machina/routes.ts:72 は `Promise.resolve([])` で常に empty | 高 |
| resolveAssignee 実際の実装 | ❌ TODO 放置 | src/machina/webhook-handler.ts:247 で user resolve ロジック zero | 高 |
| JWT_SECRET production guard | ❌ Startup 検証なし | default value で production 起動を拒否する仕組み | 高 |
| mode=none early return | ❌ 未実装 | completion check が mode=none でも実行 | 高 |
| Discussion Sessions 永続化 (Di-9) | ❌ メモリのみ | src/machina/mode-state.ts は Map<>。 再起動で消失 | 高 |
| Transaction rollback | ❌ | task create 失敗時 task_log は既に written | 中 |
| Cernere relay notification | ❌ TODO コメントのみ | src/machina/webhook-handler.ts:234 | 中 |
| CLAUDE.md | ❌ | project CLAUDE.md がない | 中 |
| CI/test automation | ⚠️ Partial | tests/core/ 充実、 MACHINA test ほぼなし、 CI 未構成 | 高 |
| Refresh token path | ❌ | JWT expires_in=900s だが refresh token なし | 中 |

## 設計上未決定事項

1. **Cernere → project-token 移行ロードマップ**: README は Composite を主説明。 project-token migration の具体的タイミング不明
2. **Discatier session state と MACHINA session state の関係**: webhook-handler → submitMessage() 経路で Discatier Core に message 注入。 しかし task-mode session (オンメモリ) と Discatier Session (Event Sourced) が別個。 source of truth は?
3. **workspace vs monitor 範囲**: completion check, N+1 query 最適化、 permission check すべて workspace 単位。 実際は monitor (channel) 単位で制限すべきか?
4. **Discatier phase2 synthesis-handlers.test.ts failure**: pre-existing (docs に明示) だが main merge 後。 修正 timeline は?

## 非機能要件

| 領域 | 現況 | 優先度 |
|------|------|--------|
| Performance | N+1 query 多数、 analysis regex 毎回 recompile | MEDIUM |
| Availability | webhook failure non-blocking は良いが、 session timeout / cleanup なし | MEDIUM |
| Scalability | SQLite single file。 write contention 増加時に問題 | LOW |
| Observability | logMachina dead、 error context 失われる、 distributed tracing なし | MEDIUM |

## 総合評価

| # | 観点 | 指摘数 | 優先度別内訳 |
|---|------|--------|-------------|
| 1 | 機能改善 | 2 | High: 0 / Medium: 2 |
| 2 | 不足機能 | 14 | 最高: 2 / 高: 7 / 中: 5 |
