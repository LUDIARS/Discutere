---
task: glab-stored-review-trends
project: Discutere
kind: 実装
created: 2026-08-25
memory_links:
  - docs/crawler-to-discussion-pipeline.md
  - spec/feature/crawler/EXTERNAL-SOURCES.md
---
# GLAB 向け取得済みレビュー傾向

## 目的

Di が取得・保存している Steam レビュー記録を正本としてゲーム別の直近傾向を分析し、
GLAB の「最近の流行り」候補へ匿名化済み集計だけを供給する。

## 分解

1. active KG の Steam attribution、発話時刻、評価反応をゲーム別に結合する。
2. 除外済み発話と集計期間外の記録を省き、直近件数・好評率・最新時刻・保存元 App ID を算出する。
3. 本文、投稿者 ID、レビュー ID を含まない Bearer 認証付き GLAB integration API を公開する。
4. 純粋集計と API 契約のテストを既存 flow test runner に追加する。

## 完了条件

- Di が保持する取得済み Steam レビューだけを分析対象にする。
- 直近 7 日の件数、評価のあるレビューに対する好評率、最新レビュー時刻を返す。
- 除外済み発話は集計に含めない。
- GLAB 向け応答にレビュー本文と個人・投稿識別子を含めない。
- 32 文字以上の `DISCUTERE_GLAB_REVIEW_TRENDS_TOKEN` を設定し、未設定時は default-deny にする。
- Di の記録が読めない場合は空の推測値を返さず、利用不可として扱う。
