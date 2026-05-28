# Phase 5: Hypothesis Lifecycle と検証パス

設計参照: `docs/discatier_implementation_plan.md` §4.3 (Hypothesis Lifecycle) / §3.3 (Axis 3 コマンド) / §7.1, §7.2 (リスクと対策)

## ゴール

Axis 3 で提案される `Hypothesis` の状態機械を実装し、 検証 Session への送付・統合・孤立検出までを通す。 ライフサイクルの状態遷移は **設計書の状態通り厳密に**実装し、 不正遷移を弾く。

## 前提

- Phase 1, 2, 4 完了 (merge hash 確認)。
- Phase 3 完了は推奨だが、 ライフサイクル自体は Phase 3 に依存しない (Translation 仮エッジは状態機械に影響しない)。

## 状態機械 (設計書 §4.3 ベース)

```
[draft] --/submit--> [proposed]
[proposed] --/validate theory--> [under_theory_validation]
[under_theory_validation] --(検証 Session 完了)--> [validated_by_theory]
[validated_by_theory] --/validate emotion--> [under_emotion_validation]
[under_emotion_validation] --(検証 Session 完了)--> [validated_by_emotion]
[validated_by_emotion] --/integrate--> [integrated]

任意状態 --/reject--> [rejected]
任意状態 (30日停滞 & 非終端) --(自動)--> [stale_flagged] (フラグのみ、 状態は維持)
```

終端状態: `integrated`, `rejected`

## ディレクトリ構成 (新規)

```
src/core/
  hypothesis/
    state-machine.ts         # 状態遷移定義 + バリデーション (人手精査対象)
    lifecycle-handler.ts     # /submit /validate /integrate /reject の本体
    validation-routing.ts    # 検証 Session 起動 + Hypothesis 参照付与
    stale-detector.ts        # 30日停滞検出ジョブ
  jobs/
    stale-detection-job.ts   # cron / オンデマンド
tests/core/hypothesis/
  state-machine.test.ts
  lifecycle.test.ts
  stale.test.ts
  e2e-proposed-to-integrated.test.ts
  e2e-stale-flag.test.ts
```

## サブタスク

1. **状態機械定義**
   - `state-machine.ts` に状態 + 遷移を const マップで定義 (型安全)。
   - `canTransition(from, action) → to | error` を export。
   - **このファイルは設計書 §4.3 を引用してコメントを残す** (人手レビューしやすく)。

2. **/submit ハンドラ拡張**
   - Phase 2 の Axis 3 `submit.ts` を呼んだ後、 lifecycle を `draft → proposed` に進める (`HypothesisProposed` event)。
   - `addresses` エッジが無ければ `submit` 自体が失敗するのは Phase 2 で実装済。

3. **/validate theory|emotion ハンドラ**
   - lifecycle を `proposed → under_theory_validation` (theory) または `validated_by_theory → under_emotion_validation` (emotion) に進める。
   - 検証 Session を新規起動 (mode は theory なら learning、 emotion なら emotion)。
   - 起動した Session に Hypothesis を参照付与する独自 edge `Session -[:validates]-> Hypothesis` を追加 (Phase 1 schema patch が必要なら同 PR で行う)。
   - `HypothesisDebated` event 発行。

4. **検証 Session 完了フック**
   - `SessionEnded` を受信したら、 当該 Session が `validates` を持つかチェック。
   - 持つ場合は `under_theory_validation → validated_by_theory` または `under_emotion_validation → validated_by_emotion` に遷移し `HypothesisValidated` event 発行。

5. **/integrate ハンドラ**
   - state が `validated_by_emotion` でなければエラー (Phase 2 で実装済の guard を再利用)。
   - `validated_by_emotion → integrated`。
   - `Hypothesis -[:integrates_as]-> Mechanic` エッジ作成 (新規 Mechanic を作るか既存に紐付けるか、 Hypothesis ペイロードで決まる)。
   - `HypothesisIntegrated` event 発行 → 該当 Mechanic に `MechanicRefined` も連鎖。

6. **/reject ハンドラ**
   - 任意状態 → `rejected`。 `HypothesisRejected` event。

7. **30日停滞検出**
   - cron 1 日 1 回 + オンデマンド。
   - 非終端状態の Hypothesis で `updated_at + 30d < now` を `stale_flagged=true` でマーク。
   - 状態自体は変更しない (フラグ列だけ更新、 `HypothesisStaleFlagged` event)。

## 受け入れ条件

- [ ] 1 つの Hypothesis が `proposed → under_theory_validation → validated_by_theory → under_emotion_validation → validated_by_emotion → integrated` まで遷移する E2E が通る。
- [ ] `/integrate` を `proposed` 状態で呼ぶと拒否される。
- [ ] `/validate emotion` を `proposed` 状態で呼ぶと拒否される (`validated_by_theory` が前提)。
- [ ] 30日経過 (テストでは時刻 mock) の非終端 Hypothesis に `stale_flagged=true` が付く。 終端 (`integrated` / `rejected`) は付かない。
- [ ] state-machine.ts の単体テストが全遷移パターンを網羅する。

## スコープ外

- `/stale` コマンドの結果表示 (Phase 6)
- ホットスポット集計 (Phase 6)

## コミット & PR

- ブランチ: `feat/discatier-phase-5-hypothesis-lifecycle`
- base: Phase 4 の merge hash (Phase 3 が並行している場合は要調整)
- PR 単位: 1 PR。 schema patch を伴う場合はその差分も同 PR 内で明示。
