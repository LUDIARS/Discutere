# モデル編成 (flow.roster) — ペルソナ別モデル + effort + Codex 混在

Spec ID: `SPEC-FLOW-MODEL-ROSTER`

> 状態: 実装済 (2026-09-05)。議論/改善フロー (`src/flow/`, dialectic / rounds 両エンジン) が対象。

## 1. 目的

議論フローは従来 `llm.model` の 1 モデルを全ペルソナに使っていた。異なるモデル・
推論深度の視点を混ぜ、生成と判定の役割ごとにモデルを選ぶ編成を config で組めるようにする。

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
  "dialectic": {
    "judgeModel": "claude-sonnet-5-5",       // 通常判定
    "finalJudgeModel": "claude-fable-5-1"    // 結論候補の評価・批准
  },
  "roster": {
    "facilitator": "claude-opus-5-5",                  // 進行役 + 止揚テキストの生成
    "discussants": [                                    // 議論者 (人数 = 要素数)
      "claude-opus-5@xhigh", "claude-opus-5@medium",
      "gpt-5.6-sol@xhigh",   "gpt-5.6-sol@medium",
      "claude-sonnet-5@high", "gpt-5.6-terra@high"
    ]
  }
}
```

env: `DISCUTERE_FLOW_ROSTER_FACILITATOR` / `DISCUTERE_FLOW_ROSTER_DISCUSSANTS` (カンマ区切り)。

2026-10-19 neco 指示による運用編成: 進行役・止揚生成は Opus 5.5、通常判定は Sonnet 5.5、
結論候補の評価・最終採否 (折衷ゲート・批准) は Fable 5.1 を維持する。
通常判定には反論の前提ルール検査、立場判定、型分類、事実照会を含む。
`finalJudgeModel` を省略または空文字にした既存設定は `judgeModel` を使う。
環境変数 `DISCUTERE_FLOW_DIALECTIC_FINAL_JUDGE_MODEL` は設定ファイルより優先する。
公式モデル一覧と Cc の有効モデル一覧を 2026-10-19 に照合した:
https://platform.claude.com/docs/en/models/overview 。モデルの公開・登録確認であり実呼出の検証ではない。
議論者のモデル編成は維持する。設定例は新規設定用で、既存の無指定時のコード既定値は変更しない。
既存環境は新しい実装の反映後に `discutere.config.json` の該当3項目を更新し、設定の再読込後に始まる議論へ適用する。
進行中の議論は開始時の設定・ペルソナを保持するため、その途中の切替は保証しない。

履歴: `fbd3cac200f960e2323b35bb59738b657e377e6c` (2026-09-05) でモデル編成を導入した際、
進行役と判定役の設定例に Fable を採用した。当時の仕様目的は「止揚判断だけ最上位モデルに任せる」
だったが、判定設定を共用する頻繁なルール検査等にも適用されていた。

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
| 前提ルール検査 / 立場判定 / 型分類 / 事実照会 | `flow.dialectic.judgeModel` |
| 結論候補の折衷ゲート / 批准 | `flow.dialectic.finalJudgeModel` (空なら `judgeModel`) |
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
