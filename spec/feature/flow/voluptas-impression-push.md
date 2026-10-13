# Voluptas からの感想の自動提供 (2026-10-13)

> neco 指示 (2026-10-13): 「Vo は匿名性を担保した上で Di に情報を提供する義務がある。
> Cr を通して Di へのデータインポートをするか、自動で行う処理を作る」

## 背景

- Discutere は Cernere 無しで単独起動する (catalog から `cernere_launch_credentials` を外した。Di #2805)。
  そのため Discutere が Cernere service token を取って Voluptas へ感想を取りに行く経路 (pull) は、
  固定トークンが無ければ使えない。
- 情報を提供する義務は Voluptas 側にある。Voluptas が **Cernere の service token で認証して**
  Discutere へ感想を **自動で送る** (push) 経路を正にする。Discutere は受けるだけで、Cernere の
  credential を持たない (検証は Cernere の公開鍵だけ)。

## 受け口 (Discutere)

`POST /api/persona-bridge/impressions` (`src/api/impression-bridge-routes.ts`)

- 認証 (`src/persona-bridge/bridge-auth.ts`、既存の persona bridge と共通):
  - Cernere service token (PASETO v4.public、aud = `CERNERE_PROJECT_STORAGE_SLUG` = `discutere`、
    scope = `persona-bridge:write`)。Voluptas は既に同じ scope で persona import / 議論開始を呼んでいる
  - 移行期間の固定トークン `DISCUTERE_PERSONA_BRIDGE_TOKEN` (32 文字以上。短い/未設定は 503)
- 本文: `{ "impressions": [ { id, gameTitle, recommend?, polarity?, comment, createdAt } ] }`、最大 200 件
- 匿名性の検査 (`src/persona-bridge/impression-intake.ts`): 上の 6 項目以外 (userId / authorId / 名前 等) を
  含む感想は 1 件ごと拒否し `skipped.non_anonymous_field` に数える。書き手は常に `anonymous` で保存する
- 取込: source=`glab`・gameSlug=`glab-<正規化したゲーム名>` で KG へ (既存 importer。同じ id は二重にならない)。
  埋め込みが有効ならベクトル化 (失敗しても取込は止めない)
- 応答: `{ ok, received, imported, embedded, games, skipped: { non_anonymous_field, invalid } }`

## 議論での使い方

ユーザーの声の収集 (`src/flow/user-voices/collect.ts`) で、Steam に無いゲームは従来どおり Voluptas から
取りに行く。取りに行けない (停止・認証不可)・0 件・Voluptas 未設定のときは、**届いていた感想 (KG 保存済み、
同じ gameSlug)** があればそれで「Voluptas の感想」として議論に混ぜる (`storedGlabCount`)。

## 送り手 (Voluptas)

Voluptas 側の自動送信 (`discutereImpressionPusher`) は Voluptas リポジトリの PR で実装する
(送る中身は既存の書き出しと同じ `glabReviewService.listImpressions` = コミュニティ公開の感想だけ・書き手の情報なし)。
