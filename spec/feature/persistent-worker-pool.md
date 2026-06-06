# 常駐ワーカープール — サブスク Lictor セッションで議論ペルソナを駆動

status: draft (2026-06-06)
branch: `feat/persistent-worker-pool`

## 1. 背景・目的

persona-engine の発話生成は従来 `ClaudeCliClient`（`claude -p` を per-fire spawn）で行っていたが:

- `claude -p` は近く従量課金化する（コスト問題）。
- Windows で git-bash パス未設定だと `claude cli exit 1` で全失敗していた（2026-06-06 時点の実障害）。

**狙い**: 発話生成を **サブスク契約の Lictor ラップ済みセッション**（= 対話 Claude Code / Codex）に載せ替え、従量 API を使わない。各ペルソナを「常駐ワーカー」として 1 セッションずつ立てておき、議論エンジンが「次に喋るペルソナ」を選んでターンを注入、ワーカーがフォーラムへ発言を返す。

実証: 2026-06-06 にスパイクで `lictor claude --model claude-opus-4-8` ＋ delegation auto-inject により Opus セッションが否定派として発言を生成しファイルへ書き戻すところまで端から端まで確認済み。

## 2. 全体像

```
                       ┌──────────────────────── Discutere (port 3100) ───────────────────────┐
                       │                                                                      │
 persona-engine        │   WorkerPoolClient (LLMClient 実装)        WorkerPool                │
   rule fire           │     invoke({personaId, system, prompt})     ├─ spawn 8 workers       │
     │ deps.llm.invoke ─┼──▶  ├─ personaId → worker 解決               ├─ /register で port 受領 │
     │                 │     ├─ reqId 採番, pending map に resolver    ├─ /v1/keys でターン注入  │
     │                 │     └─ await callback(reqId) → text          └─ health / respawn      │
     │ handleAction    │                    ▲                              │ POST /v1/keys      │
     ▼ postUtterance   │   POST /api/worker/utterance {reqId,text} ───┘     ▼                   │
 discatier-adapter ────┼──▶ onPostedUtterance → discord-hook/discussion-bridge → Di forum       │
                       └──────────────────────────────────────────────────────────────────────┘
                                              ▲ POST /register {workerId, lictorPort}
                                              │ POST /utterance {reqId, text}
        ┌── Worker N (= lictor claude/codex, LICTOR_DISABLE_CONCORDIA=1) ──┐
        │  standing persona prompt (auto-inject) で役割固定                 │
        │  起動時: 自分の $LICTOR_PORT を /register に POST                  │
        │  ターン受信(/v1/keys 注入): 発言生成 → /utterance に POST → 待機   │
        └─────────────────────────────────────────────────────────────────┘
```

要点:
- **Concordia 不使用**（`LICTOR_DISABLE_CONCORDIA=1`）。結果として **Concordia の Discord にワーカー用チャンネルが作られない**（ユーザ要件）。権限フックも付かないのでワーカーの `curl` callback が待ち無しで通る。
- 発話は Discutere 自前の `discord-hook/discussion-bridge`（Di の bot token）で forum に出る。Concordia の Discord routing は一切経由しない。
- 戻り経路は **Discutere 自前 HTTP endpoint へのコールバック**（reqId 相関）。

## 3. ペルソナ（固定8キャスト）

新ワークスペース `debate`（既定 workspace と分離。既存 `knowledge` の動的 persona 群を巻き込まない）に以下8体を seed。`personas` テーブルに `provider` / `model` 列を追加（nullable migration）。worker id = persona id。

| worker / persona id | 役割 | provider | model |
|---|---|---|---|
| `facilitator`     | ファシリテーター（止揚収束・人間優先） | claude | claude-opus-4-8 |
| `pro-opus`        | 正論派 | claude | claude-opus-4-8 |
| `con-opus`        | 否定派 | claude | claude-opus-4-8 |
| `pro-gpt`         | 正論派 | codex  | gpt-5.5 |
| `con-gpt`         | 否定派 | codex  | gpt-5.5 |
| `opinion-opus`    | 意見屋 | claude | claude-opus-4-8 |
| `opinion-sonnet`  | 意見屋 | claude | claude-sonnet-4-6 |
| `opinion-gpt`     | 意見屋 | codex  | gpt-5.5 |

各ワーカーの **standing persona prompt**（auto-inject される本文）は:
1. 役割・口調ルール（prompt-builder.ts の現行ルールを踏襲: ラベル禁止 / 一文ごと改行 / 1〜3文 / 自然口語）。
2. 動作プロトコル（register/utterance は生 curl をやめ worker-home の node スクリプト経由。後述 §4.8）:
   - 起動直後: `node scripts/register.mjs` を1回（workerId / lictorPort は env から自動取得 → `POST /internal/worker/register`）。
   - ターン受信（`[TURN] <reqId> <ターン JSON の絶対パス>` の1行注入）: 役割に沿って発言を1つ作り `replies/<reqId>.reply.json` に書き、`node scripts/send.mjs replies/<reqId>.reply.json`（→ `POST /internal/worker/utterance`、ファイルを生で送るので日本語も化けない）。それ以外は何もしない。
   - 投稿後は次のターンまで静かに待機。

## 4. コンポーネント

### 4.1 config (`src/config.ts`)
`llm.backend` に `"worker-pool"` を追加。新セクション `workerPool`:
```jsonc
"workerPool": {
  "enabled": true,
  "workspace": "debate",
  "callbackBaseUrl": "http://127.0.0.1:3100",
  "gitBashPath": "",                // 空なら自動検出
  "injectDelayMs": 2500,
  "turnTimeoutMs": 120000,
  "registerTimeoutMs": 60000,
  "workers": [ { "id":"facilitator","role":"ファシリテーター","provider":"claude","model":"claude-opus-4-8" }, ... ]
}
```

### 4.2 standing prompt 生成 (`src/persona-engine/worker-pool/persona-prompts.ts`)
worker 定義 → standing prompt 文字列。役割別テンプレ（正論派/否定派/意見屋/ファシリテーター）＋共通プロトコル節。

### 4.3 spawner (`src/persona-engine/worker-pool/spawner.ts`)
`lictor <provider-bin> --model <m> [--permission-mode acceptEdits]` を別窓 spawn（Node `child_process.spawn`）。
claude ワーカーは **auto-mode ではなく edit-mode (`acceptEdits`)** で起動し、cwd を専用ディレクトリ
`worker-home` (= `cfg.workerCwd`) にする。そこの `.claude/settings.json` で register/send スクリプトを
allow-list するので、旧来の生 curl が auto-mode 分類器に遮断される問題を回避（後述 §4.8）。codex は
`--permission-mode` が claude 専用 flag なので付けない。env:
- `CONCORDIA_DELEGATION_PROMPT_FILE` = 書き出した standing prompt md
- `LICTOR_DISABLE_CONCORDIA=1`
- `DI_WORKER_ID`, `DI_CALLBACK_URL`
- `CLAUDE_CODE_GIT_BASH_PATH`（config or 自動検出）
- `LICTOR_DELEGATION_INJECT_DELAY_MS`

### 4.4 pool (`src/persona-engine/worker-pool/pool.ts`)
- `start()`: 全 worker を spawn。`/register` で各 worker の `lictorPort` を受領するまで待機（registerTimeoutMs）。
- `dispatch(workerId, turnText) → reqId`: 該当 worker の `http://127.0.0.1:<port>/v1/keys` に `{data: turnText + Enter}` を POST。
- pending callback map（reqId → resolver）。`onUtterance(reqId, text)` で resolve。
- health: port 未登録 / dispatch 失敗 / プロセス死亡を検知して respawn（v0.1 は best-effort + ログ）。

### 4.5 client (`src/persona-engine/worker-pool/client.ts`)
`WorkerPoolClient implements LLMClient`。`invoke({personaId, system, prompt})`:
- personaId → worker 解決（無ければ error 返し engine が skip）。
- reqId 採番、pending 登録、`pool.dispatch(workerId, JSON.stringify({reqId, system, prompt}))`、`turnTimeoutMs` で reject。
- callback 到着で `{ok:true, text}`。

`LLMInvokeArgs` に `personaId?: string` を追加（engine.ts:222 が persona.id を渡す。既存 backend は無視するので後方互換）。

### 4.6 callback endpoint (`src/api/worker.ts`)
- `POST /api/worker/register {workerId, lictorPort}` → pool.registerPort。
- `POST /api/worker/utterance {reqId, workerId, text}` → pool.onUtterance。
- loopback 限定（Di の他 API と同様の guard）。

### 4.7 配線 (`src/index.ts`)
`config.llm.backend === "worker-pool"` のとき WorkerPool + WorkerPoolClient を構築し、`createPersonaEngine({ llm: workerPoolClient, workspaceId: "debate", ... })`。api ルータに `/internal/worker` を mount。`workerCwd = join(process.cwd(), "worker-home")` を pool 設定に渡す。終了時に pool.stop()（全 worker kill）。

### 4.8 worker-home（ワーカー専用 cwd） — curl 廃止 + edit-mode
ワーカー (lictor claude/codex) の cwd を Discutere 本体リポではなく専用ディレクトリ `worker-home/` にする。
これは「議論ワーカーに特殊な権限は不要」という前提のもと、旧来の生 curl が auto-mode の権限分類器に
プロンプトインジェクションとして遮断された問題への対処:
```
worker-home/
├── .claude/settings.json   # permissions.defaultMode=acceptEdits + register/send を allow-list
├── scripts/register.mjs    # 起動直後の自己 register (env: DI_CALLBACK_URL/DI_WORKER_ID/LICTOR_PORT)
├── scripts/send.mjs        # 1 ターンの発話送信 (reply JSON を生 POST → mojibake 回避)
└── replies/                # ワーカーが書く返信 JSON (gitignore)
```
- **curl → node スクリプト**: `register.mjs` / `send.mjs` を allow-list することで分類器評価なしに待ち無し実行。
- **edit-mode 起動**: spawner が claude に `--permission-mode acceptEdits` を渡す + settings.json でも宣言（二重）。
- **trust ダイアログ**: `worker-home` は `E:/Document/Ars` (trust 済) の子なので Claude Code が trust を継承し、
  headless でもダイアログで hang しない。
- **`.claude/settings.json` は手動配置が要ることがある**: 権限 allow-list を含むため、auto-mode 下の生成ツール
  からは書けず弾かれる場合がある。その場合はユーザが直接配置する（内容は `worker-home/README.md` 記載）。

## 5. ターンプロトコル / 相関

- reqId = uuid。注入 JSON `{reqId, system, prompt}`、callback `{reqId, workerId, text}`。
- timeout: turnTimeoutMs 超で pending を reject（engine は error ログ→skip、次ターンへ）。
- 二重発火防止: 同一 worker に対し前ターン未完了なら新規 dispatch を待たせる（worker busy フラグ）。

## 6. 失敗モード・既知の制約

- **権限ゲート**: `LICTOR_DISABLE_CONCORDIA=1` で permission-hook が付かない前提（wrap.ts は concordia 有効時のみ hook 付与）。さらに claude は edit-mode + worker-home の allow-list 済 node スクリプトで register/send するので待ち無しで通る（§4.8）。**Concordia 連携を切ることが前提条件**。
- **port 取得の堅牢性**: 現状は worker 自己登録（LLM が起動時に `node scripts/register.mjs` を1回）。失敗時は registerTimeoutMs で諦め respawn。将来硬化: Lictor に `LICTOR_PORT_FILE` env を足し、port をファイルに書かせて pool が deterministic に読む（Lictor 側 3 行、別 PR）。
- **codex provider**: gpt-5.5 ワーカーは Codex CLI 経由。auto-inject の submit 戦略は provider 差（codex は 2 段 submit）を Lictor が吸収済。codex は claude の settings.json / `--permission-mode` を共有しないため、register/send の node スクリプトを Codex セッション内で実行できるか（承認モード）は **要実機確認**。
- **レイテンシ**: 1ターン = 注入 + 生成（数秒〜十数秒）。常駐なので spawn コストはターンに乗らない。
- **8セッション常時占有**: サブスクのセッション枠を8消費。

## 7. 段階実装（このPRのスコープ）

- P-a: config 拡張 + persona-prompts + spawner + pool + client + endpoint（infra 一式、tsc green）。
- P-b: index.ts 配線 + `debate` workspace seed（8 persona, provider/model 列）+ 起動。
- P-c: 実機 smoke（forum へ実投稿、2ターン継続、8体起動）。

## 8. 将来の切り出し

persona-engine は既に `LLMClient` + `DiscussionContextProvider` の2 interface 依存で standalone library 化前提（context-provider.ts ヘッダ参照）。WorkerPool 一式も `LLMClient` 実装として収まるため、安定後に共有 substrate（`@ludiars/worker-pool` 等）へ昇格可能。Concordia の spawn 機構とは独立（本設計は Concordia 非依存）。
