# Phase 4: Gap Detection (Axis 1 ∩ Axis 2)

設計参照: `docs/discatier_implementation_plan.md` §4.2 (Gap Detection)

## ゴール

Axis 1 の意図 (`Mechanic.intends → Affect`) と Axis 2 の観測 (Axis 2 `Utterance.expresses → Affect`) のズレを検出し、 `DesignGap` を自動生成する。

## 前提

- Phase 1-3 完了 (merge hash 確認)。
- Translation Bridge により Axis 2 の `Utterance` が `expresses → Affect` (正規エッジ) を持つ状態が前提。

## ディレクトリ構成 (新規)

```
src/core/
  bridge/
    gap/
      detector.ts            # バッチジョブ本体
      matcher.ts             # Mechanic ↔ Utterance のマッチング
      dedup.ts               # 既存 Gap との重複チェック
      manual.ts              # 手動 Gap 作成 API (Axis 3 用)
  jobs/
    gap-detection-job.ts     # 起動エントリ (cron / オンデマンド)
tests/core/bridge/gap/
  matcher.test.ts
  detector.test.ts
  manual.test.ts
  e2e-seed-to-gap.test.ts
```

## サブタスク

1. **マッチング**
   - `Mechanic` ごとに:
     - 期待: `intends → Affect` を取得
     - 観測: 当該 Mechanic を `refers_to` で参照している Axis 2 `Utterance` の `expresses → Affect` を集約
   - 観測 Affect サンプル数 ≥ 3 のみ対象 (ノイズ閾値、 const にして調整可能に)。

2. **Gap 判定ロジック**
   - 期待 Affect が観測に含まれない → Gap 候補
   - 観測に強い負の Affect (`Affect.valence === 'negative'` 等の判定) が一定割合以上 → Gap 候補
   - 両方該当の場合は 1 件にまとめて type を `mixed` 等で記録。

3. **既存 Gap との重複チェック**
   - 同 Mechanic + 同 expected_affect + 同 observed_affect の組合せを持つ未解決 `DesignGap` があればスキップ。
   - state ∈ {open, investigating} のものを重複と見なす (resolved / dismissed は別物として再検出可)。

4. **Gap 作成**
   - 重複なしの場合 `DesignGap` ノード作成 + `gap_in / expected_affect / observed_affect / evidence` エッジ。
   - `GapDetected` event 発行。

5. **バッチジョブ起動**
   - cron 1 日 1 回 + Axis 1/2 セッション終了時のフック (`SessionEnded` 受信時) で起動。
   - 並列実行抑止 (lock ファイル or DB フラグ)。

6. **手動 Gap 作成**
   - Axis 3 用の `createManualGap({ mechanic_id, expected_affect_id, observed_affect_id, evidence_utterance_ids })`。
   - **`evidence` エッジ最低 1 必須**。 0 件ならエラー。

## 受け入れ条件

- [ ] シードデータ (Mechanic + Affect + Axis 2 Utterance × 3 件以上) を投入してジョブ実行 → 少なくとも 1 件の Gap が自動生成される (E2E)。
- [ ] 同条件で再実行しても重複生成されない。
- [ ] 観測サンプル < 3 件の Mechanic では Gap が生成されない。
- [ ] 手動 Gap 作成で `evidence` ゼロのリクエストが拒否される。

## スコープ外

- Hypothesis の自動提案 (Phase 5)
- ホットスポット集計 (Phase 6)

## コミット & PR

- ブランチ: `feat/discatier-phase-4-gap-detection`
- base: Phase 3 の merge hash
- PR 単位: 1 PR。
