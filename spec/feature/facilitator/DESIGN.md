# Facilitator — 議論の活性化と収束

> 実装: `src/persona-engine/facilitator/{facilitator.ts,prompts.ts,index.ts}`
> 設定: `config.facilitator.*` (`src/config.ts`)

議論ファシリテーター。各「議論 (discussion-of-gap session)」を独自 tick で見張り、
**停滞したら新視点を投入して広げ (expand)**、**論点が出揃ったら要約して収束させる
(converge)** per-discussion オーケストレータ。gap が closed になるまで存在し、
収束したら当該 session の管理を終える (= ファシリテーター消滅)。

投稿はすべて `contextProvider.postUtterance` を通すので、Discord relay
(およびフォーラムのスレッド) にそのまま乗る。headless 議論 (`scene=gap:*`、自動シード)
も同じ機構で駆動する (Discord に出ないだけ)。

## 駆動ループ

1 周 (`tickOnce`) で open な議論をすべて評価する。

```
tick (既定 30s 間隔)
  └ listActiveDiscussions()           ← sessions.title='discussion-of-gap:<gapId>'
       │                                 かつ scene が discord:* / gap:*、gap が未 closed
       ▼
  各議論で evaluate(metrics, state):    ← 純粋関数 (テスト可能)
       ├ 新しい発言があった       → active  (再 arm、介入しない)
       ├ idle が idleGapMs 未満    → wait
       └ idle ≥ idleGapMs かつ armed → intervene
       ▼
  intervene(d, metrics):
       ├ judgeAndStockAufhebung()      ← 止揚 (アウフヘーベン) を LLM 判定 → 新規なら stock
       ├ if 止揚数 ≥ aufhebungTarget
       │     OR distinctPersonas > maxPersonas → converge()
       └ else                                  → expand()
```

### evaluate (介入タイミング判定)

`SessionMetrics { utteranceCount, distinctPersonas, lastActivityAt, now }` と
`SessionState { lastUtteranceCount, armed }` から `active | wait | intervene` を返す純粋関数。

- 発言が増えていれば `active` にして `armed=true` に戻す (発言のたびに再 arm)。
- 発言が止まり `idleGapMs` 以上空き、かつ `armed` なら `intervene`。介入後は `armed=false`
  にして二重介入を防ぐ (次の発言で再 arm)。

## 3 つのアクション

### expand — 新視点 persona の投入 (停滞の打開)

`buildExpandPrompt` (議題 + 既存 persona 名 + 直近 12 発言) で LLM に
**新しい視点の persona** を 1 体生成させ、`dyn-<uuid8>` として `personas` に登録、
その opening を session に投稿する。既存と被らない切り口を出させて議論を広げる。

### converge — 収束まとめ + gap close

`buildConvergePrompt` で以下を束ねて要約させ、`【収束】` プレフィックス付きで投稿:

- 直近 40 発言
- stock 済みの止揚サマリ (`listAufhebung`)
- **トップ意見** (`topOpinions`): 👀 等のリアクションスコア降順。
  **人間の発言には下駄 (`HUMAN_OPINION_BASE=5`)** を載せ、人間意見を優先的に拾う。

投稿後に `designGap.update(status:'closed')` で gap を閉じ、session 管理を終了。
任意の `onConverged` フックを fire-and-forget で呼ぶ (失敗しても収束は止めない)。

### 止揚 (アウフヘーベン) のストック

`judgeAndStockAufhebung` が `buildAufhebungJudgePrompt` (議題 + 直近 16 発言 + 既存 stock)
で「対立を統合した新しい結論が生まれたか」を LLM 判定し、新規なら `aufhebung_stock` テーブル
(`id, gap_id, summary, created_at`) に積む。**止揚が `aufhebungTarget` (既定 3) たまると収束**。
= 議論が一定回数「止揚」したら自然に締める収束条件。

## 進行役 persona

`FACILITATOR_PERSONA` (`id=facilitator`、表示名「司会 結」) を起動時に
`insertOrIgnore` で登録。収束まとめの発言者であり、headless seed の開幕も担う
(seed 側と共有)。

## 設定 (`config.facilitator`)

| キー | env | 既定 | 意味 |
|---|---|---|---|
| `enabled` | `DISCUTERE_FACILITATOR_ENABLED` | `true` | 有効化 |
| `tickMs` | `DISCUTERE_FACILITATOR_TICK_MS` | `30000` | 見張り周期 |
| `idleGapMs` | `DISCUTERE_FACILITATOR_IDLE_GAP_MS` | `120000` | 「停滞」とみなす発言間隔 |
| `maxPersonas` | `DISCUTERE_FACILITATOR_MAX_PERSONAS` | `20` | persona 過多時の強制収束しきい値 |
| `aufhebungTarget` | `DISCUTERE_FACILITATOR_AUFHEBUNG_TARGET` | `3` | 止揚がこの数たまったら収束 |

## フォーラム集約との連携

`onConverged` フック (`FacilitatorConvergedEvent { gapId, sessionId, scene, title, summary }`)
を gateway の `finalizeForumPost` が受け、Discord フォーラムスレッドを **lock + archive** し、
まとめを「まとめ投稿」チャンネルへ転記する (`CLAUDE.md` フォーラム集約 / `docs/forum-aggregation.md`)。

## 関連

- 議論の素 (designGap) を立てるのは Discatier Core の gap 検出 → `spec/data/core/DESIGN.md`
- persona 駆動の返信ループ → `spec/feature/persona-engine/DESIGN.md`
