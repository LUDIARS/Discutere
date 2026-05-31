# AI に自走議論させるための設定 (LLM backend)

persona-engine が仮説提案・検証を行う際に呼ぶ LLM を選ぶ。backend は 3 種:

| backend | 説明 | 必要なもの |
|---|---|---|
| `anthropic` | Anthropic SDK で API を直叩き | `ANTHROPIC_API_KEY` |
| `claude-cli` | Claude CLI を spawn (Lictor 経由) | ローカルに `claude` CLI |
| `mock` | LLM を呼ばない (テスト / demo) | — |

`llm.backend` 未設定時の既定は `anthropic`。

## (a) Anthropic API 直叩き

```jsonc
"llm": {
  "backend": "anthropic",
  "anthropicApiKey": "",   // ← env 上書き推奨 (ANTHROPIC_API_KEY)
  "model": ""              // 未設定なら client 既定の Haiku
}
```

```sh
# env で鍵を渡す (推奨)
export ANTHROPIC_API_KEY=sk-ant-...
```

## (b) Claude CLI を spawn

API 鍵を使わず、ローカルの `claude` CLI を起動して応答を得る。

```jsonc
"llm": {
  "backend": "claude-cli",
  "claudeCliTimeoutMs": 120000,
  "gitBashPath": ""   // Windows では必須 (下記)
}
```

- **Windows では `gitBashPath` (env `CLAUDE_CODE_GIT_BASH_PATH`) が必須**。未指定だと
  CLI 起動が exit 1 で失敗する。Git for Windows の `bash.exe` パスを指す。
- `claudeCliTimeoutMs` で 1 応答のタイムアウトを調整。

## 動作確認 (LLM を実際に呼ぶ)

```sh
# MockLLM デモ (API/CLI 不要、配線確認)
npm run persona-demo

# 実 LLM デモ
export ANTHROPIC_API_KEY=sk-ant-...
npm run persona-demo -- --real-llm
```

## 自走の安全弁 (safety cap)

LLM が暴走しないよう、session 単位で発火回数に上限を設ける (config `personaEngine`):

| キー | 既定 | 意味 |
|---|---|---|
| `maxFiresPerSession` | 20 | 1 議論 session の総発火上限 (turn budget) |
| `maxFiresPerRule` | 5 | 同一 rule の発火上限 (無限ループ防止) |
| `tickMs` | 5000 | engine の tick 周期 |
| `bridgePollMs` | 2000 | events テーブルの polling 周期 |

発火カウンタは `session_rule_fires` テーブルに永続化され、再起動を跨いで cap が効く。
緊急停止は Discord で `/discutere-kill enabled:false` (admin)、解放は dashboard の Session ops。

Discord 側の設定は [discord.md](discord.md)、全キーは [config-reference.md](config-reference.md)。
