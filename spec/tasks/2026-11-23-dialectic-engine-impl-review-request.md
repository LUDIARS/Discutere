---
task: dialectic-engine-impl-review
project: Discutere
kind: 実装評価レビュー依頼 (読み取り専用)
created: 2026-11-23
requested_by: neco
reviewers: Astra / Neon / Fable (同じ依頼文で独立に。互いの成果は読まない)
base_commit: e796c5a (main)
---
# 議論エンジンの実装評価レビュー依頼

## 目的

Discutere (Di) の現在の議論エンジン (C++ 版、`native/`) の実装を評価する。
このあと次の 3 つの改修に入るので、その判断材料にする。

1. 評価 (投票など) の仕組みでトークンを無駄に使っていないか。削ってよいものと残すべきもの。
2. ペルソナとゲームのアーキタイプ (ゲームの型・遊び手のタイプ) を前提に持った発言をさせる設計
   (人物像の正本は Histrio の予定。Histrio は `E:/Document/Ars/Histrio` に設計書だけある)。
3. 議論エンジン v2 改善 (`spec/plan/2026-10-13-dialectic-v2-improvement.md`、P1〜P11)。

## 読むもの

- 仕様: `spec/feature/flow/OVERVIEW.md`, `dialectic.md`, `discussion.md`, `proxy-debate.md`, `persona-pool.md`, `model-roster.md`
- 改善設計: `spec/plan/2026-10-13-dialectic-v2-improvement.md`
- 実装 (正本は C++): `native/src/flow/dialectic/` 全体, `native/src/flow/director.cpp`, `native/src/flow/vote*.{hpp,cpp}`,
  `native/src/flow/proxy/`, `native/src/flow/persona_*`, `native/src/persona_engine/`, `native/src/config.cpp` (flow の既定値)
- 参考: `E:/Document/Ars/Histrio/README.md`, `E:/Document/Ars/Histrio/spec/feature/*.md`

`src/` の TS はサーバの正本ではない (運用スクリプト用の残り)。挙動は C++ で判断する。

## 観点 (すべてについて書く)

1. **議論の流れの正しさ**: 仕様と実装の一致。論点分割 → 立場 → 論拠の応酬 → 決着 (止揚/論破/結論なし) → 結論。食い違い・死んだ経路。
2. **LLM 呼び出しの費用対効果**: 1 論点・1 議論あたりの LLM 呼び出しの内訳 (役割ごとの回数の見積もり)。
   結果が決着・結論に効いていない呼び出し、重複している判定、安いモデルで足りる判定。投票・判定 LLM・批准・検査を特に見る。
3. **ペルソナの文脈**: 各発言者の prompt に何が載っているか (人物像・価値軸・ゲームの資料・声)。
   人物像やゲームの型を前提にした発言になっているか。アーキタイプを足すならどこに載せるのが筋か。
4. **結論の信頼性**: 撤回・崩れた論拠や未決の論点が結論に混ざる経路 (v2 設計の P1〜P3 の指摘が今も当てはまるか)。
5. **v2 改善設計の妥当性**: P1〜P11 それぞれ、実装済みか / 今も必要か / 設計に穴はないか。優先順位の提案。
6. **コード品質**: 責務分割、エラー時の degrade、テストの有無と抜け。

## 書き方

- 出力先: `spec/plan/2026-11-23-dialectic-engine-impl-review-<あなたの名前>.md` (`astra` / `neon` / `fable`) **1 ファイルだけ**。
- 冒頭に総合評価 (5 段階) と一言の理由。続けて観点 1〜6 ごとに所見。最後に「改修の優先順位 (上位 5 件)」。
- 所見には必ず根拠のファイルと行 (`native/src/...:行`) を付ける。推測は推測と書く。
- 重大度 (重大 / 注意 / 軽微) を付ける。

## 禁止

- コードの変更、commit、push、PR 作成、ブランチ・worktree の作成はしない (レビュー md 1 本を書くだけ)。
- 議論・サーバの起動、テスト・ビルドの実行、実 LLM の呼び出しはしない (読むだけ)。
- 他のレビュアーの出力ファイル (`*-impl-review-*.md`) を読まない。
- 完了は出力ファイルの実在で判定される。書き終えたら終える。
