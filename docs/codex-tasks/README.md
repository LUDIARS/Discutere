# Discatier Codex 実装タスク (Phase 1-6)

本ディレクトリは `docs/discatier_implementation_plan.md` の **新設計 (Kuzu グラフ + 3軸弁証法 + Event Sourcing)** を、 Codex が逐次実装できる粒度に分解したタスク仕様。

## 0. 前提

- **Phase 0 (人手) は完了済**: 設計書 (`docs/discatier_implementation_plan.md`) のコミット (`81c2a46`) がそれ。
- 本タスク群は **新設計 (Kuzu + 弁証法)** に対するもの。 既存の `src/ludus/` / `src/discord-hook/` (SQLite + Ludus learner) は **別レイヤ** であり、 本タスクではノータッチ。
- 新設計の実装場所は `src/core/` (仮) 配下に新規作成する想定。 既存コードと共存可能。

## 1. ワークフロー (上から順 + コミットハッシュ確認)

各 Phase は **直前 Phase の commit が main に取り込まれてから** 着手する。

```
Phase 1 → コミット → main マージ (hash X) → Phase 2 着手 (X を base に)
       → コミット → main マージ (hash Y) → Phase 3 着手 (Y を base に)
       ...
```

各 Phase の手順:

1. main から `feat/discatier-phase-N-xxx` ブランチを切る
2. 該当 Phase の md に沿って Codex が実装
3. 単体テスト + 受け入れ条件を満たすことを確認
4. PR を立てる
5. CI green + 人手レビュー後に squash merge
6. main の **マージコミットハッシュを次 Phase の base として記録** (本 README 末尾の進捗表に追記)
7. 次 Phase へ

⚠️ Phase 2 以降は前 Phase の API / 型に依存する。 必ず main へのマージ完了を待つこと。

## 2. Phase 一覧

| Phase | 目的 | タスク md | 依存 |
|-------|------|----------|------|
| 0 | 基盤設計 (人手) | — (済) | — |
| 1 | 保存層コア API | [phase-1-storage-core.md](phase-1-storage-core.md) | Phase 0 |
| 2 | メッセージ射影層 | [phase-2-message-projection.md](phase-2-message-projection.md) | Phase 1 |
| 3 | Translation Bridge (Axis 2 → Axis 1) | [phase-3-translation-bridge.md](phase-3-translation-bridge.md) | Phase 1, 2 |
| 4 | Gap Detection (Axis 1 ∩ Axis 2) | [phase-4-gap-detection.md](phase-4-gap-detection.md) | Phase 1, 2, 3 |
| 5 | Hypothesis Lifecycle | [phase-5-hypothesis-lifecycle.md](phase-5-hypothesis-lifecycle.md) | Phase 1, 2, 4 |
| 6 | 横断クエリコマンド | [phase-6-cross-queries.md](phase-6-cross-queries.md) | Phase 1-5 |

## 3. 共通ルール (全 Phase 適用)

### スコープ厳守

- **保存層への入出力契約** のみが対象。 Transport / 認証 / UI レンダリングは触らない。
- 既存 `src/ludus/` / `src/discord-hook/` / `src/machina/` の動作を壊さない。 新設計は別パスに配置。

### 不変原則 (DB レベル強制)

- `Utterance.raw_content` は作成後変更不可 (UPDATE 拒否トリガー)。
- 権限: 各軸の参加者は他軸データ参照可、 書き込みは自軸に閉じる。
- `Hypothesis.integrated` 状態への遷移は Axis 2 検証必須 (コマンドハンドラで検証)。

### Event Sourcing

- **グラフは現在のビュー、 イベントログは真実のソース**。
- 各 mutation は必ず `Event` を発行 → projection でグラフ更新の順。
- 並行して JSONL ファイル (append-only) にも書き出す。

### テスト

- 各 Phase の受け入れ条件は **テストとして実装可能** な粒度になっている。 必ずテストを書く。
- E2E は Phase ごとに最低 1 本。

## 4. Phase 進捗表 (随時更新)

| Phase | 状態 | base hash | merge hash | PR |
|-------|------|-----------|-----------|-----|
| 0 | 完了 | — | `81c2a46` (docs commit) | — |
| 1 | 未着手 | — | — | — |
| 2 | 未着手 | — | — | — |
| 3 | 未着手 | — | — | — |
| 4 | 未着手 | — | — | — |
| 5 | 未着手 | — | — | — |
| 6 | 未着手 | — | — | — |

各 Phase 完了時に Codex / 担当者は本表の `merge hash` と PR を埋める。 次 Phase 担当はこのハッシュからブランチを切る。

## 5. 参考

- 設計本体: [../discatier_implementation_plan.md](../discatier_implementation_plan.md)
- 設定 (既存): [../discatier-settings.md](../discatier-settings.md)
- Discord hook (既存): [../discatier-discord-hook-architecture.md](../discatier-discord-hook-architecture.md)
