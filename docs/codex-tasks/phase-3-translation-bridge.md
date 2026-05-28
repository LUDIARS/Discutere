# Phase 3: Translation Bridge (Axis 2 → Axis 1)

設計参照: `docs/discatier_implementation_plan.md` §4.1 (Translation Bridge)

## ゴール

Axis 2 (感情会話) の発話を埋め込み + LLM で Axis 1 の語彙 (`Affect` / `Mechanic`) に**仮**マッピングする。 自動承認はせず、 Axis 1 レビューを経て正規エッジ化する。

## 前提

- Phase 1, 2 完了 (merge hash 確認)。
- 埋め込みモデル決定済 (Phase 0)。
- `/api/llm` ラッパ or サードパーティ SDK が呼べる前提 (本タスク内で具体的に薄いラッパを実装)。

## ディレクトリ構成 (新規)

```
src/core/
  bridge/
    translation/
      pipeline.ts            # UtteranceCreated → 仮エッジ付加までのオーケストレータ
      llm-client.ts          # LLM 呼び出しラッパ (プロンプト含む)
      similarity.ts          # コサイン類似 + 上位フィルタ
      review-queue.ts        # Axis 1 「未確定マッピング」キュー
      review-handler.ts      # 承認/修正/棄却/新語彙提案 のハンドラ
prompts/
  translation/
    extract-affect.md
    extract-mechanic.md
tests/core/bridge/translation/
  pipeline.test.ts
  review-handler.test.ts
  e2e-emotion-to-review.test.ts
```

## サブタスク

1. **イベントフック**
   - Phase 1 の event bus に subscribe し、 `UtteranceCreated` & `session.mode === 'emotion'` でパイプライン起動。

2. **埋め込み生成 + 類似 Affect 検索**
   - Phase 1 の `vectors/` API を使う。
   - 上位 N (default 5) の候補 Affect を取得。

3. **関連 Mechanic 推論**
   - `session.target_game_id` から `intends → Affect` を逆引きし、 そこから到達可能な Mechanic を候補化。
   - `PlayContext` でフィルタ。

4. **LLM 判定**
   - 候補リスト + 発話 raw_content をプロンプトに入れて、 「最も妥当な Affect / Mechanic」を JSON 形式で取得。
   - プロンプトは `prompts/translation/` に保存し、 **人手調整可能**な形式 (Markdown + 明示プレースホルダ)。

5. **仮エッジ付加**
   - `Utterance -[:proposed_expresses {pending_review: true, confidence}]-> Affect`
   - `Utterance -[:proposed_refers_to {pending_review: true, confidence}]-> Mechanic`
   - 専用 EventType (`TranslationProposed`) を発行 (Phase 1 で event-types に追加してもよい — 必要なら Phase 1 patch を伴う)。

6. **未確定マッピングキュー**
   - `review-queue.ts`: pending_review=true なエッジを軸 1 ユーザに提示するための取得 API。
   - 一覧 / カーソル / フィルタ (Game / Mechanic / 時間) 提供。

7. **レビューハンドラ**
   - **承認**: `pending_review` 削除し、 仮エッジを正規 `expresses` / `refers_to` に書き換え。 `TranslationApproved` event。
   - **修正**: 別 Affect / Mechanic にマッピングし直し → 上記 + 旧仮エッジ削除。 `TranslationRevised` event。
   - **棄却**: 仮エッジ削除、 `Utterance` に `uncategorized` フラグ。 `TranslationRejected` event。
   - **新語彙必要**: `Affect` 候補ノードを `proposed` 状態で作成 + 未語彙化クラスタに集約。 `AffectAdded` event (vocabulary_status=proposed)。

## 受け入れ条件

- [ ] 感情モードの Utterance が作成されると、 仮エッジ `proposed_expresses` が自動で付加される。
- [ ] LLM 呼び出しが mock 可能 (テストで実 API 不要)。
- [ ] レビュー: **承認 → 正規エッジ化** の E2E が通る。
- [ ] レビュー: **棄却 → uncategorized フラグ** の E2E が通る。
- [ ] レビュー: **新語彙必要 → Affect (proposed) 作成 + クラスタ集約** の E2E が通る。
- [ ] プロンプトファイルが `prompts/translation/` に分離されており、 人手で編集可能。

## スコープ外

- 未語彙化クラスタの可視化 UI (Phase 6 の `/cluster` で吐く)
- Gap 検出 (Phase 4)

## コミット & PR

- ブランチ: `feat/discatier-phase-3-translation-bridge`
- base: Phase 2 の merge hash
- PR 単位: 1 PR。 ただし event-types 拡張だけ先行で Phase 1 patch する場合は別 PR。
