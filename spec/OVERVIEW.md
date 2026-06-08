# Discutere アーキテクチャ概観

**遊びの議論プラットフォーム (Discatier)** — Discord-only の自走議論 ChatBot。
本書は全体像を 5 つの観点で俯瞰し、詳細は各 DESIGN spec へ橋渡しする索引。

```
 Discord (人間 / 外部URL)
        │  MessageCreate (bot発言は破棄)
        ▼
 ┌──────────────┐   start_discussion?    ┌──────────────────────┐
 │ 分類器 #LLM① │ ─────────────────────▶ │ Discatier Core (KG)  │ designGap / hypothesis / utterance
 │ Haiku        │   record_only は記録のみ└──────────────────────┘
 └──────────────┘                                  ▲
        │ designGap 新規 → 👀                       │ read/write は context-provider 経由
        ▼                                          │
 ┌───────────────────────────────────────────────┴───────────────┐
 │ persona-engine (決定論的ルールエンジン: tick / event / cooldown)│
 │   tryFire → buildPrompt → llm.invoke → handler(action dispatch) │
 └───────────────┬───────────────────────────────┬───────────────┘
   発話 #LLM③    │ worker-pool (Lictor 常駐 CLI)  │ facilitator #LLM②
                 ▼                                ▼
        con/pro/opinion 各ペルソナ          拡張 / 収束 / 止揚 + まとめ
                 │ webhook で人間名投稿
                 ▼
            Discord (議論が流れる)
```

LLM が呼ばれるのは **3 か所だけ** (①分類器 / ②ファシリテーター / ③ペルソナ発話)。
**発火制御 (どのルールが・いつ・cooldown) は LLM ではなく `engine.ts` の決定論処理**。

---

## 1. ペルソナの起動と議論エンジンのしくみ

→ 詳細: [persona-engine/DESIGN.md](./persona-engine/DESIGN.md)

- **ルールエンジン** (`src/persona-engine/engine/engine.ts`): 1 秒 tick + `fireEvent`
  駆動。mutex 1 つで**並列禁止** (走行中の発火は `engine / skip / engine busy`)、
  `cooldown_sec` 内は無音 skip、runtime kill switch、per-session safety cap。
- **発火 → 発話の流れ** (`fireOnce`): rule.target のペルソナを引き → `buildPrompt`
  (persona identity + 議論コンテキスト JSON) → `llm.invoke` → `handler.ts` が応答
  JSON を action に dispatch (`skip` / `post_utterance` / `propose_hypothesis` /
  `add_rule` / `remove_rule`)。
- **2 種の skip ログの違い**:
  - `engine / skip` = LLM を呼ぶ手前のプログラム判定 (busy / cooldown / cap / kill)。
  - `ai / skip` = LLM を呼んだ結果、ペルソナ自身が「話すことなし」と返した判定。
- **ルール種別**: `tick` (一定間隔の自発進行) と `event` (HumanUtterance /
  DesignGapDetected 等)。seed は `seeds/rules.ts` (advocate/sceptic 等の役割系) と
  worker-pool 用 `worker-pool/debate-rules.ts` (8 キャスト)。

## 2. AI エージェント (ワーカー) の起動方法

→ 詳細: [feature/persistent-worker-pool.md](./feature/persistent-worker-pool.md)

- 各ペルソナを**サブスクの Lictor セッション (常駐 CLI ワーカー)** として起動する。
  `backend=worker-pool` 時、`WorkerPool` (`worker-pool/pool.ts`) が claude/codex CLI
  を spawn し、ワーカーは自己 register でポートを返す。
- `WorkerPoolClient.invoke({personaId})` が personaId → ワーカーへターンを注入し、
  発話 callback を待って text を返す。ターン本文は file 経由 + 1 行注入 (TUI 誤
  submit 回避)。`personaId = rule.target` でルーティング。
- ワーカー定義は `config.workerPool.workers[]` (`provider: claude|codex`, `model`)。
  `excludeProviders` でトークンの無い provider を除外可。boot 自動起動は既定 off、
  通常は `/api/worker-pool` UI から手動起動。
- **未起動ペルソナのフォールバック**: `ClaudeCliClient` が `claude -p` で代替発話
  (非 claude モデルは CLI 既定モデルに倒す)。
- **分類器 (#1)** は worker-pool を**使わず**専用 client (Haiku / Lictor or API)。
  → [feature/message-classifier.md](./feature/message-classifier.md)

## 3. ファシリテーターの機能

→ 詳細: [facilitator/DESIGN.md](./facilitator/DESIGN.md)

- 議論の流れを見て **論点を絞る / 止揚 (アウフヘーベン) で対立を統合 / 収束を促す**。
  自走 debate では `tick-facilitator-steer` ルールで定期起動、人間発言には
  `human-facilitator` (event, 短 cooldown) で最優先即応。
- **収束まとめ**: facilitator が gap を closed にすると `onConverged` が発火 →
  フォーラムスレッドを lock + archive し、まとめを「まとめ投稿」へ転記。
- フォーラムの**タグで方向を steer**: 「改善提案」系 → 課題抽出+改善案 /「面白さ」系
  → 魅力の語り合い (既定)。
- 収束まとめ等の要約は persona engine の LLM (summarizer) を使い、分類器の Haiku 化
  とは独立 (モデルを分離している)。

## 4. 議論の設計

→ 詳細: [feature/headless-auto-discussion.md](./feature/headless-auto-discussion.md)
／ [core/DESIGN.md](./core/DESIGN.md)

- **データモデル** (Discatier Core, Event Sourcing): `DesignGap` (議論の種) →
  `Hypothesis` (仮説) → `Utterance` (発話)。persona-engine は具体型を import せず
  `context-provider.ts` 経由で構造化データだけ読み書きする (切り出し可能な境界)。
- **議論の起点**: ①Discord 平文 / フォーラム starter を分類器が `start_discussion`
  と判定 → designGap 新規。②headless 自動シード (`autoSeed`: genre / store-trend を
  巡回、`maxConcurrent` 未満の時だけ新規)。③外部クロール由来。
- **進行**: pro / con / opinion ペルソナが輪番 (debate-rules、cooldown をずらして
  順番に)、人間発言が来れば最優先で応答 (`respond-to-human` / `human-*`)。
- **収束**: facilitator が止揚・収束 → gap closed → まとめ転記。
- **匿名性**: 編集者名 / アカウント名を KG・議論ノードに保存しない (匿名 workspace)。

## 5. クローラーの種類と設計

→ 詳細: [crawler/DESIGN.md](./crawler/DESIGN.md)
／ [crawler/EXTERNAL-SOURCES.md](./crawler/EXTERNAL-SOURCES.md)
／ [crawler/SENTIMENT.md](./crawler/SENTIMENT.md)

クローラーは目的の異なる **2 系統**:

- **(A) ゲーム KG クローラー** (`src/crawler/`): 著名ゲームの攻略情報を構造化抽出し
  Discatier Core の Game / Mechanic / Aesthetic ノードへ。中間形態
  `data/games/<slug>.md` (frontmatter + 自由記述) に永続化 → importer が KG へ append。
  原文転載せず要約のみ。
- **(B) 外部議論クローラー** (`crawl-channel.ts` + `crawler/EXTERNAL-SOURCES`):
  「データ学習依頼」チャンネルに貼られた URL を `classifyCrawlUrl` で取得元種別へ
  分類して取り込む。種別:
  - **steam** (`store.steampowered.com` / `steamcommunity.com` の `/app/<id>`)
  - **youtube** (`watch?v=` / `youtu.be/` / `shorts|live|embed`)
  - **reddit** (`/comments/<id>`)
  - **website** (その他 URL、長文は LLM summarizer で要約/raw 2 層)
  外部の声を要約 + raw の 2 層で取り込み、議論のシード / 学習データに回す。感情極性は
  SENTIMENT で別途付与。

---

## 設定とエントリポイント

- 設定は `src/config.ts` の単一 typed config に集約 (default < `discutere.config.json`
  < env)。LLM backend は `llm.backend` (`claude-cli`/`anthropic`/`worker-pool`/`mock`)、
  分類器は独立した `classifier.{backend,model,timeoutMs}`。
- 起動配線は `src/index.ts`。Discord Gateway は常時 WS 接続 (`discord-hook/gateway.ts`)、
  認証は Discord 依存 (bot token + admin-id allowlist、Cernere 非使用)。
