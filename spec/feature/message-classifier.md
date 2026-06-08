# メッセージ分類器 (投稿 → 議題化判定)

Discord に投稿された非アプリ発言を「議論を起こすべきか / 記録だけか」へ振り分ける
判定器。`classifyDiscordMessage` (`src/discord-hook/auto-discussion.ts`) が本体。

## 位置づけ — 議論ループ内の 3 つの LLM 判定のうちの 1 つ

Discutere で LLM が呼ばれるのは次の 3 か所だけ。「どのルールが発火するか / cooldown /
tick」は `engine.ts` が**決定論的に**処理しており LLM ではない (→ [persona-engine
DESIGN](../persona-engine/DESIGN.md))。

| # | 判定 | 実装 | モデル方針 |
|---|------|------|-----------|
| 1 | **分類器** — 投稿→議題化 (start_discussion / record_only、category) | `classifyDiscordMessage` | **Haiku** (本 spec) |
| 2 | ファシリテーター — 拡張 / 収束 / 止揚 + 収束まとめ | `src/facilitator/` | 据え置き (persona engine LLM) |
| 3 | ペルソナ発話 — skip 判断 + 発話本文を 1 コールで両方 | prompt-builder→llm.invoke→handler | worker-pool (Opus/GPT/Sonnet) |

分類器は判定の最前段であり、ここで `record_only` になれば後段のペルソナ / facilitator
の LLM は一切走らない。よって**軽量モデルで速く・安く回す**のが要件。

## 入力経路 (非アプリ発言だけを拾う)

Discord Gateway の `MessageCreate` で **bot 発言は取り込み前に破棄** される
(`gateway.ts`: `if (msg.author?.bot) return;`)。ペルソナの発話は webhook (bot) なので
ここに乗らない。つまり分類器に届くのは **人間 / 外部由来の非アプリ発言だけ**。

- 平文議論チャンネル (`discord.discussionChannelIds`) と フォーラム starter は
  `routeInboundMessage` / `routeForumPost` → `classifyInboundMessage` 経由で分類器へ。
- フォーラムスレッドのタイトルは議論の主題 (対象ゲーム) として `threadTitle` で渡る。

## バックエンド — Haiku を Lictor 経由 / API のどちらでも

分類器は **persona engine の LLM (worker-pool) とは独立した専用 client** を持つ
(`buildClassifierLlm()` in `src/index.ts`)。

> なぜ分離するか: persona の `WorkerPoolClient` は `personaId` ルーティング前提。
> 分類呼び出しは personaId を持たないため、共用すると `ok:false` → regex fallback に
> 落ち、LLM が一切効かなくなる (本 spec 導入前の隠れた挙動)。また分類器を Haiku 化
> しても facilitator (#2) のモデルには影響させない、という分離も同時に満たす。

`config.classifier` (env / `discutere.config.json`):

| キー | 既定 | 説明 |
|------|------|------|
| `backend` | `claude-cli` | `claude-cli`=Lictor 経由 spawn (サブスク・トークン不要) / `anthropic`=API 直 / `off`=LLM 無効 (regex のみ) |
| `model` | `claude-haiku-4-5-20251001` | 分類モデル。精度優先なら Sonnet 等に変更可 |
| `timeoutMs` | `30000` | claude-cli backend のタイムアウト (分類は短文なので短め) |

env: `DISCUTERE_CLASSIFIER_BACKEND` / `DISCUTERE_CLASSIFIER_MODEL` /
`DISCUTERE_CLASSIFIER_TIMEOUT_MS`。`backend` 未指定時は `ANTHROPIC_API_KEY` があれば
`anthropic`、無ければ `claude-cli` に倒す (CLI が無い CI/headless での後方互換)。

- **claude-cli** = `ClaudeCliClient` が `claude -p --model <haiku>` を spawn (Lictor
  wrapping 配下と同等の挙動)。生テキストを返すので action ラップは無い。
- **anthropic** = `AnthropicSdkClient` が `/v1/messages` を `model=haiku` で叩く。

## 出力スキーマ

LLM は次の JSON のみを返す (周辺説明は許容、最初の `{...}` を抽出)。

```json
{
  "action": "start_discussion | record_only",
  "category": "game_design_question | mechanic_question | opinion | noise | command_like",
  "title": "短い日本語タイトル",
  "description": "日本語要約",
  "expectedAffect": "optional",
  "observedAffect": "optional",
  "reason": "短い理由"
}
```

`start_discussion` のときだけ designGap を新規に立て、👀 リアクションを付ける
(= 議論が立った合図)。

## フォールバック (regex)

LLM 不在 / 失敗 / `backend=off` のときは `classifyDiscordMessageFallback` が
キーワード規則で判定する (`?？` / 疑問詞 / ゲーム用語 → start_discussion、`/`・URL
のみ → command_like、信号不足 → noise)。**LLM 経路が死んでも議題化は止まらない**。

## 関連

- [persona-engine DESIGN](../persona-engine/DESIGN.md) — 決定論的な発火エンジンと
  ペルソナ発話 (#3)
- [facilitator DESIGN](../facilitator/DESIGN.md) — 拡張 / 収束 / 止揚 (#2)
- [headless-auto-discussion](./headless-auto-discussion.md) — 自走シードとの関係
- [crawler EXTERNAL-SOURCES](../crawler/EXTERNAL-SOURCES.md) — 外部由来の非アプリ入力
