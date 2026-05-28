# REVIEW_QUALITY — Discutere (2026-05-29)

評価: **B**

## 1. テスト戦略・カバレッジ

| 評価 | 観点 | 所見 |
|------|------|------|
| A | Discatier Core phase 1~6 unit テスト | `tests/core/hypothesis/`, `tests/core/bridge/`, `tests/core/projection/` 合 24 ファイル。 state machine, stale detector 等 critical logic covered |
| D | MACHINA layer unit test | webhook-handler, analyzer, task-mode の unit test ほぼなし |
| D | E2E test | analyzer regex, due-date extraction, completion detection の e2e なし |
| C | CI 自動化 | npm scripts (test:core, test:phase2 等) あるが GitHub Actions workflow 未構成。 phase2 failing で main merge 許容 |
| D | 境界値テスト | 大型 message、 重複 webhook 再送、 同時性 stress test なし |

### action items

- GitHub Actions + `npm run test:phase*` 追加
- MACHINA layer basic test coverage (最低 analyzer.ts regex)

## 2. ライセンス遵守

| 依存 | ライセンス | 状態 |
|------|----------|------|
| hono | MIT | OK |
| drizzle-orm | Apache 2.0 | OK |
| jsonwebtoken | MIT | OK |
| bcryptjs | MIT | OK (dead dep — 下記参照) |
| @discordjs/ws | Apache 2.0 | 新規追加必要 (PR #18 scope) |

**問題**:
- `package.json:33` bcryptjs dependency があるが code で import なし (dead dependency)
- LICENSE ファイル存在 (MIT)
- README に "Dependencies" 節がなく、 ユーザが license compliance 確認しにくい

**推奨**:
1. `npm uninstall bcryptjs`
2. README に "## Dependencies" 節追加
3. Discatier Core 新依存 (kuzu graph DB 模倣等) も明記

## 3. ドキュメント完備性

| 評価 | 観点 | 所見 |
|------|------|------|
| A | README 最新性 | project overview, tech stack, API endpoint list, getting started は概ね最新 |
| B | DESIGN 文書 | `src/machina/PLAN.md` は Discutere 独立前の path のため outdated。 Discatier Core は別途設計書なし |
| D | CLAUDE.md | なし。 new contributor の onboarding 困難 |
| C | Inline comments | Discatier Core は JSDoc 充実、 MACHINA layer は希薄 (routes.ts 特に) |
| B | API/interface | README §「API Endpoints」で REST endpoint 列挙、 Discatier Core の event type / projection command は code only |
| B | Contributing guide | .github/CONTRIBUTING.md なし |
| D | Runbook / troubleshooting | dev 開始時 Cernere 依存、 env-cli 初期化が必須だが debug 経路 README に未記載 |

### 個別文書状態

- ✓ `README.md` (2026 最新)
- ✓ `docs/discatier_implementation_plan.md` (510 lines, 詳細)
- ✓ `docs/discatier-discord-hook-architecture.md` (53 lines)
- ✓ `docs/codex-tasks/2026-05-28-discord-only-pivot.md` (140 lines)
- ⚠️ `src/machina/PLAN.md` (outdated, path mismatch)
- ❌ `CLAUDE.md` (なし)
- ❌ `DESIGN.md` (なし。 全体 architecture digest 必要)

## CI 自動化状態

- ✗ GitHub Actions workflow 未構成
- ⚠️ npm scripts はあるが CI 未接続
- ⚠️ phase2 synthesis-handlers.test.ts failing でも main merge 許容

## 変更履歴 / CHANGELOG

- ⚠️ CHANGELOG.md なし
- ⚠️ git commit message 一部詳細だが希薄なものもある
- ⚠️ 各 phase completion status を追うには docs/codex-tasks/ を見る必要

## 4. クロスプラットフォーム互換

| 評価 | 観点 | 所見 |
|------|------|------|
| B | OS | Node.js 22+。 Windows / macOS / Linux 共通 |
| B | パス | path.join() で normalize。 ただし一部 hardcode あり |

## 総合評価

| # | 観点 | 評価 | 重大指摘数 |
|---|------|------|-----------|
| 1 | テスト戦略・カバレッジ | C | 2 (MACHINA layer test なし, CI 未構成) |
| 2 | ライセンス遵守 | B | 1 (bcryptjs dead dep) |
| 3 | ドキュメント完備性 | B | 3 (CLAUDE.md なし, PLAN.md outdated, runbook なし) |
| 4 | クロスプラットフォーム互換 | B | 0 |

**Discatier Core 追加効果**: test coverage 大幅増 (0 → 24 ファイル)
**MACHINA layer 悪影響**: webhook-handler, task-mode のロジック複雑化に対し test 不在
