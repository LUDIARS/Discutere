# 完成済みディスカッションペーパーの自動議論開始

status: implemented (2026-07-16)

## 目的

Omnipotensなど外部の解析パイプラインが作成したディスカッションペーパーを、Discutere側で再調査・再編集・人間承認待ちに戻さず、その内容を正本として議論開始できるようにする。

## API

`POST /api/flow/start-from-paper`

```json
{
  "paperMd": "# 議題\nMagicChronicleは面白く、売れる設計か",
  "flow": "discussion",
  "tags": ["開発"],
  "rounds": 3,
  "turnsPerRound": 4,
  "personaIds": ["pro-gpt", "opinion-sonnet"]
}
```

- `paperMd`: 必須。互換名として `bodyMd` も受理する。
- `theme`: 任意。未指定時は標準の `# 議題`、それも無ければ先頭H1をテーマにする。
- `flow`: 任意。既定は `discussion`。`discussion` / `improvement` のみ許可する。
- `tags`: 任意。Discutereの有効タグだけを採用する。
- `rounds` / `turnsPerRound` / `personaIds`: 通常のフロー開始と同じ任意指定。

成功時はHTTP応答を待たせずバックグラウンドで議論を開始する。

```json
{
  "ok": true,
  "kind": "discussion",
  "sessionId": "...",
  "autoStarted": true
}
```

## 境界と安全策

- 完成済みペーパー向けの入口なので、情報ゲート、クロール、Webペーパーレビューは省略する。
- 入力Markdownは書き換えず `bodyMd` の正本として議論参加モデルへ渡す。
- 空本文、テーマを解決できない本文、学習・壁打ちフローはHTTP 400で拒否する。
- 議論処理自体は既存の `dispatchFlow` と `paperOverride` を再利用し、永続化・完了状態・エラー処理を通常経路と共通化する。
