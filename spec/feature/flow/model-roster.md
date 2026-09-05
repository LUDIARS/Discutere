# モデル編成 (flow.roster) — ペルソナ別モデル + effort + Codex 混在

Spec ID: `SPEC-FLOW-MODEL-ROSTER`

> 状態: 実装済 (2026-09-05)。議論/改善フロー (`src/flow/`, dialectic / rounds 両エンジン) が対象。

## 1. 目的

議論フローは従来 `llm.model` の 1 モデルを全ペルソナに使っていた。異なるモデル・
推論深度の視点を混ぜ、**止揚判断だけ最上位モデルに任せる**編成を config で組めるようにする。

## 2. model spec

`"<model>@<effort>"` の 1 文字列でモデル ID と effort を運ぶ (`src/persona-engine/llm/model-spec.ts`)。

| 例 | 経路 | CLI/API への落とし方 |
|---|---|---|
| `claude-opus-5@xhigh` | Claude (SDK → `claude -p`) | SDK: `output_config.effort` / CLI: `--effort xhigh` |
| `claude-fable-5-1` | Claude | effort 未指定 = API 既定 |
| `gpt-5.6-sol@medium` | Codex (`codex exec`) | `-m gpt-5.6-sol -c model_reasoning_effort="medium"` |

`mid`/`med` は `medium` の別名。effort の語彙は provider に従う
(claude: low/medium/high/xhigh/max、codex: minimal/low/medium/high/xhigh)。

## 3. 設定

```jsonc
"flow": {
  "dialectic": { "judgeModel": "claude-fable-5-1" },   // 判定・批准 (止揚の採否)
  "roster": {
    "facilitator": "claude-fable-5-1",                 // 進行役 + 止揚テキストの生成
    "discussants": [                                    // 議論者 (人数 = 要素数)
      "claude-opus-5@xhigh", "claude-opus-5@medium",
      "gpt-5.6-sol@xhigh",   "gpt-5.6-sol@medium",
      "claude-sonnet-5@high", "gpt-5.6-terra@high"
    ]
  }
}
```

env: `DISCUTERE_FLOW_ROSTER_FACILITATOR` / `DISCUTERE_FLOW_ROSTER_DISCUSSANTS` (カンマ区切り)。

## 4. 割当規則 (`generateFlowPersonas`)

- facilitator = `roster.facilitator` (空なら `llm.model`)。
- 議論者 (opinion → debater の生成順) に `discussants` を先頭から順に割り当てる。
  人数が足りなければ巡回。`discussants` が空なら全員 `llm.model` (従来挙動)。
- 人数: `discussants` が 1 つ以上なら **1 + 要素数** で各モデルをちょうど 1 回使う
  (`rosterPersonaCount`)。空なら `flow.personaCount`。
- `personaIds` で指定したプールペルソナは自身の `model` を保つ (未設定なら `llm.model`)。

## 5. 止揚の役割分担 (dialectic)

| 処理 | モデル |
|---|---|
| 論点分解 / 進行文 / 止揚テキストの生成 | `roster.facilitator` (`mainModel`) |
| 型分類 / 事実照会 / 折衷ゲート / 批准 | `flow.dialectic.judgeModel` |
| 立場表明 / 発話 | 各ペルソナの spec |

## 6. 経路の振り分け (`src/flow/model-router.ts`)

`ModelRouterLlm` が `gpt-` 接頭辞を `CodexCliClient` (`src/persona-engine/llm/codex-cli.ts`)
へ、それ以外を既存チェーン (worker-pool → SDK(OAuth) → `claude -p`) へ回す。
Codex 経路の失敗は Claude にフォールバックしない (GPT の意見を Claude が代弁すると偽装になる)。
`codex exec` は `-s read-only --ephemeral --skip-git-repo-check`、cwd は `worker-home/`、
prompt は stdin、最終文は `-o` ファイルで受ける。子プロセスの環境変数は実行と
サブスクリプション認証に必要な OS / home 系だけを allowlist し、サービスの資格情報・
内部 endpoint・セッション変数は継承しない。

## 7. 既知の制約

- コスト推定 (`pricing.ts`) は Claude ファミリのみ。gpt-* は usage を返さないので 0 扱い。
- worker-pool (常駐 Lictor セッション) 経路は personaId ルーティングのため、
  生成ペルソナ (UUID) には当たらない。roster の Codex は常に `codex exec` one-shot。
