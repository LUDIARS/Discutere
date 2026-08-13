# STEAM-PERSONA — Steam 横断レビュアーの検出と集中収集

- Spec-ID: SPEC-STEAM-PERSONA-PIPELINE / SPEC-STEAM-PERSONA-STORE / SPEC-STEAM-PERSONA-OUTPUT
- 起点: neco 指示 (2026-08-13)。「新作のレビューのうちレビュー件数が 200 を超えるものを定期的に
  取得し、同一 ID が複数のゲームに対して投稿しているのを検出する。その後その ID のレビューを
  集中的に集め、ペルソナの元データとする。これを自動的に収集するツールを用意する。」
- 関連: EXTERNAL-SOURCES.md §4.1 (steam collector) / §6 (persona アンカー) 、
  Canalis `src/crawl/steam/` (①adapter) 、Voluptas persona-engine v2 (下流の消費側候補) 。

## 1. 役割分担

| 層 | 置き場 | 責務 |
| --- | --- | --- |
| ① 取得 | Canalis `@ludiars/canalis/steam` | 新作一覧 / アプリ別レビュー / ユーザ別レビューの決定論取得 (API キー不要・LLM なし) |
| ② 制御・状態 | Di `src/crawler/steam-persona/` | 閾値判定・増分管理・横断検出・集中収集の指揮と sidecar 永続化 |
| 出力 | `data/external/steam-persona/authors/<steamid64>.jsonl` | ExternalUtterance 互換のペルソナ元データ (Vo / Hi が消費) |

意見集約 (議論) は Di、ペルソナ形成は Voluptas / Histrio 側の責務 (neco 2026-08-13)。
本パイプラインは「元データを自動で貯める」までを担い、KG への取り込みは行わない
(追跡アプリは Game ノードを持たないため。必要になれば既存 `ext-import` に JSONL を渡せる形)。

## 2. パイプライン (SPEC-STEAM-PERSONA-PIPELINE)

1 実行 (`npm run steam-persona`) で以下を 1 周する。定期実行は Concordia timer delegation が
このコマンドを叩く (Di 常駐プロセス・cron には依存しない)。

1. **発見** — store 検索 (リリース日降順) の新作 `--max-apps` 件を走査し、
   レビュー総数が `--min-total-reviews` (既定 **200**) を**超える**未追跡アプリを
   `tracked_app` に登録する。既追跡アプリの再判定はしない (呼び出し節約)。
2. **定期取得** — 追跡アプリごとに appreviews (filter=recent) を増分取得する。
   前回先頭の `recommendationid` で打ち切り、(app_id, steam_id) の出現を
   `author_sighting` に冪等記録する。匿名 (steamid 欠落) は対象外。本文は保存しない。
3. **横断検出** — `author_sighting` を集計し、`--min-cross-apps` (既定 **2**) 本以上の
   追跡アプリに投稿した steam_id を `cross_author` に登録する。登録済み ID は再検出しない。
4. **集中収集** — 未収集の横断投稿者を `--max-authors` (既定 10) 人まで、
   steamcommunity プロフィールの全レビューを取得して JSONL に書き出し、収集済みにする。
   非公開プロフィールは 0 件 (エラーにしない)。

polite delay (既定 1200ms/リクエスト) は Canalis 側が担う。

## 3. 状態 (SPEC-STEAM-PERSONA-STORE)

sidecar SQLite `data/external/.steam-persona.sqlite` (utterances / KG schema 不変)。

- `tracked_app` — app_id / title / total_reviews / first_seen_at / last_checked_at /
  last_recommendation_id (増分打ち切り位置)
- `author_sighting` — (app_id, steam_id) PK / recommendation_id / seen_at
- `cross_author` — steam_id PK / app_count / detected_at / collected_at / collected_reviews

## 4. 出力 (SPEC-STEAM-PERSONA-OUTPUT)

投稿者ごとに `data/external/steam-persona/authors/<steamid64>.jsonl`。
1 行 = ExternalUtterance 互換 (source="steam" / nativeId=`ur:<steamid>:<appid>` /
gameSlug=`steam-app-<appid>` / authorId=SteamID64)。

- authorId (SteamID64) は EXTERNAL-SOURCES §6 の統治原則「情報精度 > プライバシー」に従い
  保管層でフル保持する。露出時のマスクは消費側 (`maskedPersonaLabel`) の責務。
- プロフィールページの投稿日はロケール依存文字列しか取れないため `postedAt` は取得時刻。
  原文の `postedText` が要る場合は Canalis adapter の raw を使う (将来拡張)。

## 5. 非ゴール

- ペルソナ形成そのもの (Vo / Hi)。
- プラットフォーム横断の同一人物紐付け (EXTERNAL-SOURCES §10 と同じ)。
- レビュー本文の全アプリ常時収集 (集中収集は検出済み投稿者のみ)。
