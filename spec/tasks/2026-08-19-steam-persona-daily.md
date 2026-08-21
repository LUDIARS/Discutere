---
task: steam-persona-daily
project: Discutere
kind: 運用
created: 2026-08-19
memory_links:
  - feedback_steam_persona_error_handling
---

# Steam 横断レビュアー収集 — 毎朝

## 目的

Discutere steam-persona 定期実行 (Concordia timer delegation) の日次実行を記録する。
毎朝自動で `npm run steam-persona` を 1 周回し、Steam 新作レビューから横断投稿者を検出・集中収集する。

## 作業内容

- `npm run steam-persona` を Discutere cwd で実行
- stdout の JSON サマリ (scanned / trackedNew / trackedTotal / sightings / detected / collectedAuthors / collectedReviews / failedAuthors) を取得
- 検出・収集の進度を記録

## 実行結果 (2026-08-20)

```json
{
  "scanned": 200,
  "trackedNew": 5,
  "trackedTotal": 26,
  "sightings": 6606,
  "detected": 106,
  "collectedAuthors": 10,
  "collectedReviews": 0
}
```

| メトリクス | 値 |
|-----------|-----|
| スキャン新作 | 200 |
| 新規追跡アプリ | 5 (追跡合計 26) |
| レビュー出現数 | 6,606 |
| 横断投稿者検出 | 106 人 |
| 集中収集完了 | 10 人 |
| 取得レビュー数 | 0 件 (プロフ非公開) |
| JSONL ファイル | 10 ファイル (新規) |

> この実行は `failedAuthors` 追加前のため同フィールドを含まない。当時は
> 「取得失敗で 0 件」と「公開プロフィールだがレビュー 0 件」がサマリ上区別できず、
> 上記の「プロフ非公開」はログからの推定。以降の実行は `failedAuthors` で判別できる。

## 受け入れ条件

- timer delegation が毎朝起動し、サマリが取得できる
- `.steam-persona.sqlite` に tracked_app / author_sighting が増分記録される
- `data/external/steam-persona/authors/` に JSONL が累積される
- エラー (fetch 失敗) は graceful degrade してループを継続
