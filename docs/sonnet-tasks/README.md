# 議論フロー (4 フロー) 実装タスク — Sonnet 用指示書

`spec/flow/` の 4 フロー (議論 / 改善 / 学習 / 壁打ち) を Sonnet が逐次実装するための
タスク分解。設計の正本は **`spec/flow/OVERVIEW.md` + 各フロー spec**。本ディレクトリは
それを実装可能な粒度に割ったもの。

## 0. 前提・方針

- **設計判断は spec で確定済**。実装者 (Sonnet) は spec と本指示書に従って書く。spec と矛盾
  する判断が要る場合は**勝手に決めず、指示書の「未決事項」に追記して質問**する。
- **persona-engine (ルールエンジン) は議論フローでは使わない**。新規 `FlowDirector` を作る。
  persona-engine は削除せず参考材料として残す (OVERVIEW §4)。
- **まず議論フロー (T1→T3) を完成させる**。改善/学習/壁打ち (T4-T6) はその後。

## 1. タスク一覧と依存

| # | タスク | 指示書 | 依存 | フェーズ |
|---|---|---|---|---|
| T1 | 共通基盤 (設定 + タグ + DB スキーマ + コスト/transcript ログ) | [t1-foundation.md](t1-foundation.md) | — | 議論 |
| T2 | 議論フロー: FlowDirector + ディスカッションペーパー + 調査/YouTube | [t2-discussion-core.md](t2-discussion-core.md) | T1 | 議論 |
| T3 | 議論フロー: 投票 + 感情ベクトル評価 + ラウンドサマリ/止揚 + 結論 | [t3-discussion-vote-conclude.md](t3-discussion-vote-conclude.md) | T1, T2 | 議論 |
| T4 | 改善フロー (design_gap 機械スコア) | [t4-improvement.md](t4-improvement.md) | T2, T3 | 改善 |
| T5 | 学習フロー (議論前のユーザ意見/メカニクス収集) | [t5-learning.md](t5-learning.md) | T1 | 学習 |
| T6 | 壁打ちフロー (ユーザターン起点・上限付き) | [t6-sparring.md](t6-sparring.md) | T2, T3 | 壁打ち |
| T7 | 起動経路 (簡素 WebUI + Discord/Slack トリガ + フロー選択) | [t7-entrypoints.md](t7-entrypoints.md) | T2 | 横断 |

> T2 はテスト用に最小トリガ (dev 用 slash か関数直叩き) を含めてよい。正式な 2 経路の入口は T7。

## 2. 共通ルール (全タスク適用)

### スコープ厳守
- **指示書のスコープ (in)** のみ実装する。「ついでに」便利機能・無関係リファクタを足さない。
- 既存の persona-engine / Discatier Core / crawler の動作を壊さない。新規は新パスに置く。

### コード規約
- LUDIARS 共通規約に従う (正本 `AIFormat/RULE_CODE.md` / skill `coding-conventions`)。
  **単一責任 (SRP) とファイル分割は必須**。1 ファイルに責務を詰め込まない。
- 設定は `src/config.ts` の typed config に集約 (default < `discutere.config.json` < env)。
  シークレットは平文保存しない。

### DB マイグレーション
- 新規カラム用 **`CREATE INDEX` は `ALTER ADD COLUMN` の後**に冪等発行する (既存 DB で
  "no such column" boot 失敗を避ける)。
- boolean を `=== 1` でシリアライズする箇所は、追加 boolean カラムも漏れなく同処理に入れる。

### エラー処理 (OVERVIEW §10)
- LLM / 外部取得 / 投票 / 要約の失敗は**握り潰さない**。**エラーログを出す + 発話にもエラーを
  提示**する。空応答 (skip) とエラーは区別する。

### テスト
- 受け入れ条件は**テストとして書ける粒度**。各タスクで単体テスト + 最低 1 本の結合/E2E。
- LLM 依存部は `MockLLMClient` (既存) でテストする。外部 API (YouTube) は mock/fixture で。

### dev server / 実行
- dev server は既定で立てない (`feedback_no_dev_server`)。疎通は test / mock で行う。

### ブランチ・PR
- 各タスクは `feat/flow-<name>` ブランチ + PR。`origin/main` 最新から切る。
- CI green を確認してから squash merge + ブランチ削除 + main 同期。マージ完了は origin 実体で
  裏取りする。次タスクはマージ後の main から着手。

## 3. 進捗表 (随時更新)

| # | 状態 | 実装 | 備考 |
|---|---|---|---|
| T1 | ✅ 完了 | `src/flow/{config,tags,db,cost-logger}` | PR #113 (Sonnet) |
| T2 | ✅ 完了 | `src/flow/{director,discussion-paper,investigate,personas}` | PR #114 (Sonnet) |
| T3 | ✅ 完了 | `src/flow/{vote,round-summary,conclusion}` | PR #115 / fix #116 (Sonnet) |
| T4 | ✅ 完了 | `src/flow/{design-gap,improvement-score,improvement,sentiment-vector}` | runFlow に RoundEvaluator 注入を追加 (Opus) |
| T5 | ✅ 完了 | `src/flow/{learning,games-md}` + `ExternalSource "feedback"` | Opus |
| T6 | ✅ 完了 | `src/flow/{sparring,sparring-commands}` | コマンドは文字列一致に確定 (Opus) |
| T7 | ✅ 完了 | `src/flow/{dispatch,entry-discord,web/}` + index.ts 登録 | gateway 配線は follow-up (Opus) |

> T4-T7 は Opus が一括実装 (元は Sonnet 委託想定だったが速度都合で変更)。spec/flow + 本指示書も
> 同 PR で main へ載せた (T1-T3 マージ時には未マージだった)。

### 実装時の確定事項 / 残課題
- **T6 コマンド検出**: 分類器でなく**文字列一致**に確定 (短い命令文限定で誤検知抑制、敬語ゆらぎ許容)。
- **T7 Discord 配線**: `entry-discord.ts` は純パーサ + ハンドラまで。`gateway.ts` の ThreadCreate への
  実配線は既存フォーラム経路 (`routeForumPost`) を壊さないよう follow-up とした。
- **T7 RAG**: WebUI 起動時の `listExternalVoices` 配線は未実施 (flow は未配線でも空で動作)。follow-up。
- **学習 slug**: `toSlug` は ASCII のみ残すため日本語タイトルは `deps.slug` 明示が必要。

## 4. 参照

- 設計正本: [`../../spec/flow/OVERVIEW.md`](../../spec/flow/OVERVIEW.md) +
  [discussion](../../spec/flow/discussion.md) / [improvement](../../spec/flow/improvement.md) /
  [learning](../../spec/flow/learning.md) / [sparring](../../spec/flow/sparring.md)
- 既存基盤: `spec/persona-engine/DESIGN.md` / `spec/facilitator/DESIGN.md` /
  `spec/crawler/SENTIMENT.md` / `spec/feature/discussion-party.md` / `spec/feature/web-chat.md`
