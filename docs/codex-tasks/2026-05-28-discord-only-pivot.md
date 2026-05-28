# Discutere Discord-only pivot — タスク退避メモ

**作成**: 2026-05-28
**コンテキスト**: セッション `[Di] タスク退避 (Cernere無し)`
**完了済**: PR #17 (Phase 5/6 hypothesis lifecycle + cross queries) merged @ `9d89624` / PR #18 (Di-14 Cernere + frontend 撤去) open

## 背景の決定事項

1. **Cernere は不要**。 Discutere は Discord chat-only サービスで完結させる。 [[project_personal_data_rule]] は Discord platform 識別子 (user_id / display_name) を対象外と解釈する。
2. **API はスラッシュコマンドで role-based**。 Discatier 設計 `docs/discatier_implementation_plan.md §3.1-§3.4` と整合 (theorist / player / developer)。
3. **MACHINA admin REST は過渡期のみ残す**。 X-User-Id / X-User-Role ヘッダーだけで動かし、 Di-1/Di-5/Di-10 で slash command が引き継いだら削除。
4. **Frontend は廃止**。 議論結果の参照は (a) Discord 内 slash + embed、 (b) GitHub Discussions、 (c) 月次 Markdown/JSONL の 3 経路。

## タスクリスト (推奨着手順)

```
Done : Di-14 [PR #18 open] Cernere/Frontend 撤去
基盤 : Di-1  Discord Interactions endpoint + Bot Gateway 接続
       Di-5  3軸 channel-mode 拡張 + thread=Session bind
       Di-9  Session 状態を SQLite 永続化
データ: Di-4  Phase 0 初期 Affect 語彙を seed
       Di-3  Embedding 生成パイプラインを接続
AI   : Di-2  LLM client (Anthropic) を実装し MockLlmClient を切替可能に
       Di-8  3軸別 Bot 応答テンプレ (prompt pack)
出力 : Di-10 横断 slash command の Discord embed builder        ← C1
       Di-11 Discatier 用 GitHub Discussions publisher           ← C2
       Di-12 月次 Markdown/JSONL export + Praeforma reference 規約 ← C4
副次 : Di-6  MACHINA summarizer を LLM 化し論点抽出を追加
       Di-13 Memoria への語彙同期 API (低優先)                    ← C3
```

## タスク詳細

### Di-1 [#9] Discord Interactions endpoint + Bot Gateway 接続
REST 露出ではなく Discord 経路で Discatier Core を駆動。
- `POST /api/discord/interactions` (Ed25519 署名検証 + slash command 受信)
- `@discordjs/ws` で Gateway 常時接続 + intent: `GuildMessages`
- 受信した interaction / message を `createCore()` の projection layer に転送
- `/health` のみ HTTP 残す。 MACHINA admin REST は Di-14 で X-User-Id 受付の縮退済 → slash 移行が終わるまで残す

### Di-2 [#10] Anthropic LLM client
- `MockLlmClient` (`src/core/bridge/translation/llm-client.ts:10`) と並列に `AnthropicLlmClient.extract()` を追加
- prompt は `src/core/bridge/translation/prompts/*.md` に分離
- `ANTHROPIC_API_KEY` 不在時は MockLlmClient に fallback
- Translation Bridge `pipeline.ts` の DI を差し替え可能に

### Di-3 [#11] Embedding 生成パイプライン
- `src/core/vectors/embedding.ts` は格納のみ → text→vector を作る側を追加
- voyage-3 か text-embedding-3-small を選択 (LUDIARS 他で使用済みのものに揃える)
- `generateEmbedding(text)` を新設
- UtteranceCreated 受信時に自動 embed → `registerEmbedding` する hook を Phase 1 layer に追加
- `vector-search.ts` と統合確認

### Di-4 [#12] Phase 0 初期 Affect 語彙 seed
- `docs/discatier_implementation_plan.md §9` の 15 語 (ワクワク/達成感/没入/発見/共感/ゾクッ/ホッ/スッキリ/イライラ/ダルい/困惑/拍子抜け/理不尽/もどかしい/飽きた) を起動時に `AffectAdded` イベント経由で投入
- 冪等な seed (`vocabulary_status=established`, `source='phase0'`)
- unit test で「起動後 affect が 15 件以上ある」 を確認

### Di-5 [#13] 3軸 channel-mode + thread=Session bind
- `monitor.discatier_axis ∈ {learning, emotion, synthesis}` 追加 (現 task/discussion/none と共存)
- `Session` schema に `thread_id` 列追加 + thread 作成時に Session を起こす
- Discord guild role → Discatier role (`theorist`/`player`/`developer`) resolver
- slash command 前段の role guard middleware (axis × role mismatch は deny)
- `/me role` コマンドで role を Discord guild role 経由で切替表示

### Di-6 [#14] MACHINA summarizer を LLM 化
- `src/machina/summarizer.ts:8` のコメント通り、 ルールベース要約を Haiku/Sonnet に差し替え
- 返り値を `{summary, points: {agreed[], disputed[], open[], next_actions[]}}` に拡張
- 既存呼び出し点 (`discussion-mode.ts`) は新 shape を解釈
- GitHub Discussion body と channel notice にも論点セクションを追記
- LLM 不在時は現在のルールベース挙動を保持 (fallback)

### Di-8 [#16] 3軸別 Bot 応答テンプレ
- Axis 1 (learning): 構造化候補提示 + 既存 Mechanic/Affect 引用 + 未語彙化クラスタ通知
- Axis 2 (emotion): 最小受身、 理論用語禁止、 メタ情報隠す
- Axis 3 (synthesis): スレッド冒頭に Gap 要約 pin + Hypothesis 状態通知 + `/propose` 検出時に構造化
- `src/core/bridge/translation/prompts/axis-{1,2,3}.md` に分離、 Di-2 の AnthropicLlmClient から DI

### Di-9 [#17] Session 状態を SQLite 永続化
- `src/machina/mode-state.ts` の `discussionSessionStore` はオンメモリ Map → 再起動で消失
- `discussion_sessions` テーブル新設 (id, monitor_id, status, scheduled_at, window_start, error_reason, last_published_url)
- `_timer` は永続化せず、 起動時に pending status の session を読み戻して `setTimeout` 再アタッチ
- Discatier Session 側も Event Sourcing (Event テーブル) と SQLite projection の整合 routine を整備

### Di-10 [#18] 横断 slash command の Discord embed builder ★出力 C1
- `/lineage <mechanic>` 定義変遷を embed で thread に返す
- `/affect dictionary` `vocabulary_status=established` の Affect 一覧
- `/gap list state:open` open Gap dashboard
- `/hypothesis state:<proposed|debated|integrated|...>`
- `/cluster` 未語彙化感情クラスタ
- `/stale` 30 日停滞 Hypothesis
- `/hotspot` Gap 集中 Mechanic
- 各 command を `src/core/projection/command-handlers/cross/` の既存実装と接続、 Discord embed の整形のみ追加

### Di-11 [#19] Discatier 用 GitHub Discussions publisher ★出力 C2
- `src/machina/github-discussion.ts` の publisher を再利用しつつ Discatier 用 wrapper を追加
- トリガ:
  - (a) Discatier Session 終了 (thread closed) → `[Axis-N] <thread title> discussion log`
  - (b) Mechanic 定義 refine 時 → `Mechanic <name> v<n>`
  - (c) Hypothesis integrated 時 → `採用された仮説 <title> — 根拠 chain 付き`
- 各 publisher は対象 monitor の `githubRepo` + category に分けて投稿
- post 失敗は session を failed にせず Event だけ残す (Translation Bridge 等の継続性確保)

### Di-12 [#20] 月次 Markdown/JSONL export + Praeforma 規約 ★出力 C4
- cron で月次 (or on-demand `/export YYYY-MM`) に以下を生成:
  - `docs/discatier/results/<YYYY-MM>/mechanics.md` (Mechanic 定義 snapshot + lineage 抜粋)
  - `docs/discatier/results/<YYYY-MM>/affect.jsonl` (`vocabulary_status=established`)
  - `docs/discatier/results/<YYYY-MM>/hypothesis.md` (integrated 仮説 + 根拠)
  - `data/events/<YYYY-MM>.jsonl` (Event テーブル全件 — 真実のソース ダンプ)
- Praeforma reference proxy 規約: spec 内 `{{mechanic:WaveSpawner}}` 等で参照可能に。 [[project_praeforma]] 側に Issue を立てる
- git で diff 追跡できる

### Di-13 [#21] Memoria への語彙同期 API (低優先) ★出力 C3
- Memoria 側に `POST /api/domain-dict/discatier` endpoint 新設 (Memoria に Issue)
- Discutere 側 sync job (`Affect.vocabulary_status=established` か `Mechanic` refine イベントで差分送信)
- Di-11/Di-12 が先、 これは横展開要求が出たら着手

## 出力経路の 4 軸まとめ (再掲)

| | C1 Discord 内 (Di-10) | C2 GitHub Discussions (Di-11) | C4 Markdown/Praeforma (Di-12) | C3 Memoria (Di-13) |
|---|---|---|---|---|
| AI 学習量 | ★★ | ★★ | ★★★ | ★★★ |
| 作業コスト | ★★ | ★ | ★★ | ★★★ |
| 目的達成度 | ★★★★★ | ★★★★ | ★★★ | ★★★ |
| 主目的一致度 | ★★★★★ | ★★★★ | ★★★★ | ★★★ |

**推奨順**: C1 → C2 → C4 → C3。

## 既知の懸念

- **phase2 synthesis-handlers.test.ts:27 が main で失敗中**。 PR #17 由来の pre-existing failure。 Di-1 着手前に別 Issue / 別 PR で修正したい
- **Lictor session のタイトル更新が文字化け**する場面あり (`/rename` 経由の OSC が `???` 化)。 [[project_lictor]] 側で原因調査が必要だが、 機能には影響なし
- **`bash.exe.stackdump` がリポ直下に残る**。 .gitignore に追加 + 既存ファイルは削除推奨 (今回は scope 外、 別 PR で)

## 次回再開時のスタート手順

1. PR #18 (Di-14) の merge 状況確認
2. main 同期
3. このファイルを再読
4. Di-1 から着手 (推奨)、 または優先タスクを user に確認
