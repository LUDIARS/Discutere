# M3: MACHINA (タスク自動生成) モジュール計画書

## 概要

Slack/Discord のチャットログを監視し、タスクが必要な文脈を自動検出してタスクを生成・更新するモジュール。Claude Code Haiku による構文解析とルールベース解析のハイブリッドで動作し、グループに属する形でタスクを管理する。M2「PM」モジュールへのリレー機能を持ち、プロジェクト管理と連携する。

---

## モジュール構成

```
modules/machina/
├── PLAN.md                     # 本設計書
├── routes.ts                   # メイン API ルート
├── analyzer.ts                 # テキスト解析エンジン (ルールベース + AI)
├── webhook-handler.ts          # Slack/Discord Webhook 受信 & タスク自動生成
└── pm-relay.ts                 # M2「PM」リレーインターフェース (アダプタパターン)
```

---

## 1. チャンネル監視 & タスク自動生成

### 1.1 監視対象

| プラットフォーム | 受信方式 | メッセージ形式 |
|----------------|---------|--------------|
| Slack | Event API (Webhook) | `event_callback` → `message` イベント |
| Discord | Webhook (Bot) | メッセージオブジェクト (`content`, `author`, `mentions`) |

### 1.2 グループ別チャンネル監視

グループごとに複数のチャンネルを監視設定できる。`machina_channel_monitors` テーブルで管理。

```
[Slack/Discord メッセージ受信]
    │
    ▼
┌──────────────────────┐
│  Webhook Receiver     │  POST /api/machina/webhook/slack
│  (routes.ts)          │  POST /api/machina/webhook/discord
└──────────┬───────────┘
           ▼
┌──────────────────────┐
│  Channel Filter       │  machina_channel_monitors テーブルで
│  (webhook-handler.ts) │  監視対象チャンネルか判定
└──────────┬───────────┘
           ▼
┌──────────────────────┐
│  Text Analyzer        │  ルールベース解析 → タスク判定
│  (analyzer.ts)        │  (将来: Claude Haiku API 解析)
└──────────┬───────────┘
           ▼
┌──────────────────────┐
│  Task Generator       │  confidence >= 0.5 の場合タスク作成
│  (webhook-handler.ts) │  アサイン・優先度・納期を自動設定
└──────────┬───────────┘
           ▼
┌──────────────────────┐
│  PM Relay             │  M2 接続時は自動リレー
│  (pm-relay.ts)        │  未接続時はスキップ (stub)
└──────────┬───────────┘
           ▼
┌──────────────────────┐
│  emitEvent()          │  通知モジュール経由で通知
│  → machina.task.*     │  Webhook チャネルで配信
└──────────────────────┘
```

### 1.3 コマンドによるタスク作成

チャットで明示的にタスクを作成するコマンドパターン:

| コマンド | 説明 | 信頼度 |
|---------|------|--------|
| `!task <タイトル>` | タスク作成 | 1.0 (確定) |
| `/task <タイトル>` | タスク作成 | 1.0 (確定) |
| `!machina <タイトル>` | タスク作成 | 1.0 (確定) |

### 1.4 通知イベント

既存の通知モジュール (`modules/notification/`) の `emitEvent()` を利用。

```typescript
// EVENT_NAMES (src/shared/constants.ts に追加)
MACHINA_TASK_CREATED:   "machina.task.created"     // タスク自動生成
MACHINA_TASK_UPDATED:   "machina.task.updated"     // タスク自動更新
MACHINA_TASK_COMPLETED: "machina.task.completed"   // タスク自動完了
MACHINA_TASK_ASSIGNED:  "machina.task.assigned"    // アサイン自動変更
MACHINA_TASK_RELAYED:   "machina.task.relayed"     // PM (M2) へリレー
```

### 1.5 API エンドポイント

```
# タスク管理
GET    /api/machina/groups/:groupId/tasks              # タスク一覧 (?status= でフィルタ)
GET    /api/machina/groups/:groupId/tasks/:taskId       # タスク詳細 + ログ
POST   /api/machina/groups/:groupId/tasks              # タスク手動作成
PUT    /api/machina/groups/:groupId/tasks/:taskId       # タスク更新
DELETE /api/machina/groups/:groupId/tasks/:taskId       # タスク削除

# タスクログ
GET    /api/machina/groups/:groupId/tasks/:taskId/logs  # 変更履歴

# PM リレー
POST   /api/machina/groups/:groupId/tasks/:taskId/relay # PM (M2) へ手動リレー

# チャンネル監視
GET    /api/machina/groups/:groupId/monitors            # 監視設定一覧
POST   /api/machina/groups/:groupId/monitors            # 監視追加
PUT    /api/machina/groups/:groupId/monitors/:id        # 監視更新
DELETE /api/machina/groups/:groupId/monitors/:id        # 監視削除

# Webhook 受信 (認証不要: Slack/Discord からの受信)
POST   /api/machina/webhook/slack                       # Slack Event API
POST   /api/machina/webhook/discord                     # Discord Webhook

# ユーティリティ
POST   /api/machina/analyze                             # テキスト解析プレビュー
GET    /api/machina/status                              # モジュール状態
```

---

## 2. 自動設定

### 2.1 納期自動設定

テキストの文脈から納期を判断する。パターンマッチで以下の表現を検出:

| パターン | 解決先 |
|---------|--------|
| `今日中` / `本日中` / `today` | 当日 23:59 |
| `明日まで` / `明日中` / `tomorrow` | 翌日 23:59 |
| `M/D` / `M月D日` | 指定日 23:59 (過去なら翌年) |
| `N日後` / `N日以内` / `N days` | N日後 23:59 |
| `今週中` / `this week` | 今週金曜 23:59 |
| `来週まで` / `来週中` / `next week` | 来週金曜 23:59 |

### 2.2 アサイン自動判断

テキストの文脈とメンバーの役割からアサインを判断:

**メンション解決:**
- Slack: `<@U12345>` → ユーザーID 抽出
- Discord: `mentions` 配列から直接取得
- テキスト内 `@username` → グループメンバーの名前/メールアドレスで照合

**完了検出によるアサイン切り替え:**

以下のキーワードが検出されると、発言者にアサインされた `in_progress` / `pending` タスクを `done` に自動更新:

```
修正した / push / マージ / merge / デプロイ / deploy
クローズ / close / finished / 対応済み / resolved / done / 完了
```

### 2.3 優先度自動設定

テキストのキーワードから優先度を判定:

| 優先度 | キーワード |
|--------|-----------|
| `critical` | 急ぎ / 至急 / ASAP / urgent / 緊急 / 今日中 / すぐ / ブロッカー / blocker / critical / 障害 |
| `high` | 重要 / important / 高優先 |
| `medium` | (デフォルト) |
| `low` | できれば / 余裕があれば / 低優先 / low priority |

**クリティカルパス判定 (将来拡張):**

M2「PM」のバックログと連携し、依存関係グラフからクリティカルパス上のタスクを自動判定する。現在は手動設定 (`isCriticalPath` フラグ)。

---

## 3. M2「PM」へのリレー

### 3.1 アダプタパターン

M2 モジュールが未実装の間は stub として動作し、M2 完成後にアダプタを差し替えて接続する。

```typescript
// pm-relay.ts
interface MachinaPmRelay {
  createTask(task: MachinaTask): Promise<{ pmTaskId: string }>;
  updateTask(pmTaskId: string, updates: Partial<MachinaTask>): Promise<void>;
}

// M2 側が初期化時にアダプタを登録
registerPmRelayAdapter({
  async createTask(task) {
    // pm_tasks テーブルへの挿入
    return { pmTaskId: createdTask.id };
  },
  async updateTask(pmTaskId, updates) {
    // pm_tasks テーブルの更新
  },
});
```

### 3.2 リレーフロー

```
[タスク自動生成 / 手動作成]
    │
    ▼
┌──────────────────────┐
│  machina_tasks に保存  │
└──────────┬───────────┘
           ▼
┌──────────────────────┐
│  PM アダプタ登録済み?  │──── No ──→ [スキップ (ログ出力)]
└──────────┬───────────┘
           │ Yes
           ▼
┌──────────────────────┐
│  relayTaskToPm()      │  アダプタ経由で M2 に送信
└──────────┬───────────┘
           ▼
┌──────────────────────┐
│  relayedToPm = true   │  リレー済みフラグ & pmTaskId を記録
│  pmTaskId = result.id  │
└──────────────────────┘
```

### 3.3 手動リレー

自動リレーされなかったタスクは、API 経由で手動リレー可能:

```
POST /api/machina/groups/:groupId/tasks/:taskId/relay
```

---

## テキスト解析エンジン

### 現行: ルールベース解析

パターンマッチと正規表現でタスク検出を行う:

**タスク検出パターン:**

| パターン | 例 |
|---------|-----|
| `タスク: ...` / `task: ...` | 「タスク: ログイン画面の修正」 |
| `TODO: ...` | 「TODO: テスト追加」 |
| `お願い: ...` / `...をお願い` | 「ボタンの色変更をお願い」 |
| `...してください` / `...して欲しい` | 「APIのエラーハンドリングを追加してください」 |
| `やること: ...` / `必要: ...` | 「やること: デプロイスクリプトの更新」 |
| `バグ: ...` / `issue: ...` | 「バグ: ログインが動かない」 |
| `修正: ...` / `実装: ...` / `追加: ...` | 「修正: 日付バリデーション」 |

**信頼度:**
- コマンド (`!task`, `/task`): 1.0 (確定)
- パターンマッチ: 0.6
- 完了キーワード検出: 0.7
- 閾値: 0.5 未満はタスク生成しない

### 将来: Claude Haiku API 解析

ルールベースで信頼度が低い場合に Claude Haiku API を呼び出し、より高精度な解析を行う:

```typescript
// analyzer.ts (Phase 2 拡張)
async function analyzeWithHaiku(input: AnalysisInput): Promise<AnalysisResult> {
  const response = await anthropic.messages.create({
    model: "claude-haiku-4-5-20251001",
    max_tokens: 1024,
    messages: [{
      role: "user",
      content: buildAnalysisPrompt(input),
    }],
  });
  return parseHaikuResponse(response);
}

function buildAnalysisPrompt(input: AnalysisInput): string {
  return `
以下のチャットメッセージを分析し、タスク管理の観点で判断してください。

## メッセージ
プラットフォーム: ${input.platform}
送信者: ${input.authorName}
テキスト: ${input.text}
メンション: ${input.mentions?.join(", ") || "なし"}

## 判断してほしいこと
1. タスクを作成すべきか (shouldCreateTask: boolean)
2. 既存タスクの更新か (shouldUpdateExisting: boolean)
3. タスクタイトル (title: string)
4. 優先度 (priority: low/medium/high/critical)
5. アサイン先のヒント (assigneeHint: string | null)
6. 納期のヒント (dueDateHint: ISO 8601 | null)
7. 信頼度 (confidence: 0.0-1.0)
8. 判定理由 (reasoning: string)

JSON で回答してください。
`;
}
```

---

## DB スキーマ設計

### machina_channel_monitors

| カラム | 型 | 説明 |
|--------|-----|------|
| `id` | TEXT PK | UUID |
| `groupId` | TEXT FK → groups.id | グループID |
| `platform` | TEXT | `"slack"` \| `"discord"` |
| `channelId` | TEXT | チャンネルID (Slack/Discord) |
| `channelName` | TEXT | 表示用チャンネル名 |
| `webhookEndpointId` | TEXT | 関連 Webhook 設定ID (nullable) |
| `isActive` | BOOLEAN | 有効/無効 |
| `createdBy` | TEXT | 作成者ユーザID |
| `createdAt` | TIMESTAMP | 作成日時 |
| `updatedAt` | TIMESTAMP | 更新日時 |

**UNIQUE 制約:** `(groupId, platform, channelId)`

### machina_tasks

| カラム | 型 | 説明 |
|--------|-----|------|
| `id` | TEXT PK | UUID |
| `groupId` | TEXT FK → groups.id | グループID |
| `title` | TEXT | タスクタイトル |
| `description` | TEXT | タスク詳細 (nullable) |
| `status` | TEXT | `"pending"` \| `"in_progress"` \| `"done"` \| `"cancelled"` |
| `priority` | TEXT | `"low"` \| `"medium"` \| `"high"` \| `"critical"` |
| `assigneeId` | TEXT | アサインされたユーザーID (nullable) |
| `dueDate` | TEXT | 納期 ISO 8601 (nullable) |
| `source` | TEXT | `"auto"` \| `"command"` \| `"manual"` |
| `sourcePlatform` | TEXT | `"slack"` \| `"discord"` \| null |
| `sourceMessageId` | TEXT | 生成元のメッセージID (nullable) |
| `sourceChannelId` | TEXT | 生成元のチャンネルID (nullable) |
| `sourceText` | TEXT | 生成元のメッセージテキスト (最大2000文字, nullable) |
| `confidence` | INTEGER | AI 解析信頼度 (0-100) |
| `isCriticalPath` | BOOLEAN | クリティカルパスフラグ |
| `relayedToPm` | BOOLEAN | PM (M2) リレー済みフラグ |
| `pmTaskId` | TEXT | PM 側タスクID (nullable) |
| `createdBy` | TEXT | 作成者ユーザID |
| `createdAt` | TIMESTAMP | 作成日時 |
| `updatedAt` | TIMESTAMP | 更新日時 |

**インデックス:** `groupId`, `status`, `assigneeId`, `dueDate`, `priority`

### machina_task_logs

| カラム | 型 | 説明 |
|--------|-----|------|
| `id` | TEXT PK | UUID |
| `taskId` | TEXT FK → machina_tasks.id | タスクID |
| `action` | TEXT | `"created"` \| `"updated"` \| `"assigned"` \| `"status_changed"` \| `"priority_changed"` \| `"relayed"` |
| `previousValue` | TEXT (JSON) | 変更前の値 (nullable) |
| `newValue` | TEXT (JSON) | 変更後の値 (nullable) |
| `reason` | TEXT | 変更理由 / AI 判定の根拠 (nullable) |
| `triggerMessageId` | TEXT | トリガー元メッセージID (nullable) |
| `performedBy` | TEXT | 実行者: `"system"` (自動) \| ユーザID |
| `createdAt` | TIMESTAMP | 作成日時 |

**インデックス:** `taskId`

---

## フロントエンド設計

### ページ構成

| パス | ページ | 内容 |
|------|--------|------|
| `/machina` | `MachinaPage.tsx` | タスク管理 / 監視設定 / テキスト解析プレビュー |

### タブ構成

| タブ | 内容 |
|------|------|
| タスク | タスク一覧 (ステータスフィルタ) + 詳細パネル + 作成フォーム |
| 監視設定 | チャンネル監視の CRUD |
| テキスト解析 | テキスト入力 → 解析結果プレビュー (デバッグ・検証用) |

### API 定義 (`frontend/src/lib/api.ts`)

```typescript
export const machinaApi = {
  // Tasks
  getTasks: (groupId, status?) => request<MachinaTaskListResponse>(...),
  getTask: (groupId, taskId) => request<MachinaTaskDetailResponse>(...),
  createTask: (groupId, data) => request<{ id: string; message: string }>(...),
  updateTask: (groupId, taskId, data) => request<{ message: string }>(...),
  deleteTask: (groupId, taskId) => request<{ deleted: string }>(...),
  getTaskLogs: (groupId, taskId) => request<MachinaTaskLogListResponse>(...),
  relayTask: (groupId, taskId) => request<{ message: string; pmTaskId?: string }>(...),

  // Channel Monitors
  getMonitors: (groupId) => request<MachinaMonitorListResponse>(...),
  createMonitor: (groupId, data) => request<{ id: string; message: string }>(...),
  updateMonitor: (groupId, monitorId, data) => request<{ message: string }>(...),
  deleteMonitor: (groupId, monitorId) => request<{ deleted: string }>(...),

  // Analysis & Status
  analyzeText: (text, platform?) => request<MachinaAnalysisResponse>(...),
  getStatus: () => request<MachinaStatusResponse>(...),
};
```

---

## 既存モジュールとの統合

### 通知モジュール (`modules/notification/`)

- `emitEvent()` でイベント発火 → 既存の Webhook 配信パイプラインで配信
- `EVENT_NAMES` に MACHINA 用イベントを追加
- `EVENT_MODULES` に MACHINA モジュールのイベントグループを追加

### グループモジュール (`modules/group/`)

- タスクはグループに属する (`groupId` FK)
- グループメンバーシップでアクセス制御
- グループメンバー情報でアサイン先を解決

### M2「PM」モジュール (`modules/pm/`)

- アダプタパターンで疎結合接続
- M2 の `pm_tasks` テーブルにタスクをリレー
- M2 のクリティカルパス分析結果を MACHINA に反映 (将来)

### app.ts への登録

```typescript
import { machinaRoutes } from "../modules/machina/routes.js";

app.route("/api/machina", machinaRoutes);
```

### CLAUDE.md への追記

```
| `modules/machina/` | `frontend/src/pages/MachinaPage.tsx` | `api.ts` の `machinaApi` |
```

---

## 実装フェーズ

### Phase 1: 基盤 & ルールベース解析 (実装済み)
1. DB スキーマ追加 (`machina_channel_monitors`, `machina_tasks`, `machina_task_logs`)
2. リポジトリ関数追加 (`machinaChannelMonitorRepo`, `machinaTaskRepo`, `machinaTaskLogRepo`)
3. ルールベーステキスト解析エンジン (`analyzer.ts`)
4. Slack/Discord Webhook 受信 & タスク自動生成 (`webhook-handler.ts`)
5. PM (M2) リレーインターフェース (`pm-relay.ts`) — stub
6. REST API ルート (`routes.ts`)
7. フロントエンド: MachinaPage (タスク管理/監視設定/解析プレビュー)
8. 通知イベント統合

### Phase 2: Claude Haiku AI 解析
1. Anthropic SDK 統合 (`claude-haiku-4-5-20251001`)
2. ルールベース信頼度が低い場合に Haiku にフォールバック
3. コンテキストウィンドウ活用 (直近N件のメッセージ履歴を含めた解析)
4. プロジェクトロール情報をプロンプトに含めたアサイン精度向上

### Phase 3: M2「PM」連携
1. M2 モジュール完成後にアダプタ接続
2. PM のタスク依存関係グラフと連携したクリティカルパス自動判定
3. PM のバックログバックトラックによる優先度の動的調整
4. 双方向同期 (PM 側の変更を MACHINA に反映)

### Phase 4: 高度な自動化
1. スレッド追跡: Slack スレッドの会話フローからタスクのサブタスク自動生成
2. 重複検出: 既存タスクとの類似度計算による重複タスク防止
3. 学習フィードバック: ユーザーがタスクを承認/却下した結果を解析精度にフィードバック
4. 定期レポート: グループ別のタスク生成統計レポート
