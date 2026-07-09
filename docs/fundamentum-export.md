# 議論データ管理ツール (Fundamentum export)

2026-07-09。議論 (paper + 発話 + 結論) を **個別に export** できる管理ツールとして、
LUDIARS 共通データ基盤 **Fundamentum (`Fm`, `lib/fundamentum` submodule)** を組み込む。

## 目的

既存の md エクスポート (`GET /learning/conclusion/export`, `npm run export:discussions`) は
「人間が読む1ファイル」を書き出すだけで、バージョン管理・重複排除・機械可読な再取込には向かない。
Fundamentum を使うと:

- **content-addressed**: 議論の内容が変わらなければ再 export しても同じ content id → 重複書込みなし。
- **catalog によるバージョン管理**: 議論ごとに `discussionId → content id` を版で持ち、
  再 export で内容が変わった版だけ履歴に残る (`getDiscussionExportHistory`)。
- **個別取り出し可能**: 1 議論だけを content id で取得できる (全件 dump 不要)。

## 実装

`src/fundamentum-export/`:

- `store.ts` — `Foundation.onDisk(config.fundamentum.dataDir)` の遅延シングルトン。
- `snapshot.ts` — `ConclusionDetail` (`src/visualize/conclusions.ts`) を正規化 JSON へ変換する純関数。
  **`exportedAt` のような「いつ export したか」は含めない** — 混ぜると毎回別 content id になり
  content-addressed の dedup が壊れる。
- `export.ts` — `exportDiscussionToFundamentum(detail)`。snapshot を `master.put` → catalog
  `discutere/discussions` に `discussionId` (= 既存の `gapId`/`flow:<sessionId>`) で `set` → `commit`。
- `list.ts` — `listExportedDiscussions()` / `getExportedDiscussionSnapshot(id)` /
  `getDiscussionExportHistory(id)` (その議論の content id が版を跨いで変わった箇所だけ抽出)。

## 入口

- **CLI**: `npm run export:discussions:fm` (= `scripts/export-discussion.ts --fundamentum`)。
  `--id <gapId>` で 1 件、無指定で全件。
- **Web UI**: `/learning/conclusions` の各議論カードに「Fundamentum へ export」ボタン
  (`POST /learning/conclusion/export/fundamentum?gap=<id>`)。
- **設定**: `config.fundamentum.enabled` (既定 true) / `config.fundamentum.dataDir`
  (既定 `./data/fundamentum`、env `DISCUTERE_FUNDAMENTUM_ENABLED` / `DISCUTERE_FUNDAMENTUM_DATA_DIR`)。

## データ配置とバックアップ

`data/fundamentum/` は他の `data/*` 同様 cwd 相対 (Electron は userData chdir 後に解決、
`docs/desktop-app.md` 参照)。`config.fundamentum.enabled` なら `src/backup/runner.ts` の
バックアップ対象に自動で含まれる。

## セットアップ

`lib/fundamentum` は git submodule (private repo)。ローカルは `npm run setup:submodules` で
init + build される (CLAUDE.md ルート README 参照)。
