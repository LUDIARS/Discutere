# ユーザーの声の収集 (Steam / Voluptas + 類似ゲームの均等混合)

> 状態: 実装済み (2026-10-07)。議論/改善の準備 (Discord フォーラム経路) で動く。

## 目的

ディスカッションペーパーと議論に渡す「ユーザーの声」を、議題のゲームに実際に触れた人の声で揃える。

- 議題のゲームが **Steam でリリース済み** なら、Steam のレビューを取得して KG に取り込み、ベクトル化する。
- **Steam に無い** (見つからない / 未リリース) なら、Voluptas の「遊んだ感想」
  (Voluptas `GET /api/personas/impressions`、SPEC-GLAB-IMPRESSION-EXPORT) を取り込み、ベクトル化する。
- **類似ゲーム** の指定があれば、類似ゲームの声も同じ手順で集め、**ゲームごとに同じ件数ずつ混ぜる**。

## 流れ (`src/flow/user-voices/`)

| 段 | ファイル | 中身 |
|---|---|---|
| ゲームの解決 | `steam-app.ts` | ストア検索 (`/api/storesearch`) の候補のうち、名前が議題 (または指定名) に含まれるものを採用し、`appdetails` の `release_date.coming_soon` でリリース判定 |
| Voluptas | `voluptas-impressions.ts` | 感想を匿名の ExternalUtterance (source=`glab`、authorId は固定の匿名値) にする。認証はペルソナ取り込みと同じ Bearer |
| 収集 | `collect.ts` | ゲームごとに Steam → (無ければ) Voluptas → 取込 → ベクトル化。失敗は warn して次へ (議論を止めない) |
| 引き出し | `game-voices.ts` | 取込時の gameSlug (attribution) でゲーム単位に引き、議題の埋め込みに近い順 (無ければ新しい順) |
| 混合 | `mix.ts` | ゲームごとに 1 件ずつ交互に取り出す。足りないゲームの分は他で埋め、同じ本文は 1 回だけ |
| 入口 | `runtime.ts` | 既定の実装を束ね、スレッドの外部の声検索を均等混合に差し替える |

- ゲーム単位で引くのは、Steam レビュー・感想が本文にゲーム名を書かないことが多く、議題の語による
  キーワード検索では見つからないため。
- 議題のゲームの声が集まらなかった場合、議題側は既存のキーワード検索で埋める (類似ゲームの声だけにはしない)。
- ベクトル化は `config.embedding.enabled` のときだけ行う。無効なら取込だけ行い、新しい順で使う。
  ベクトル化の本体は `src/core/vectors/voice-index.ts` (offline バッチ `npm run build:voice-embeddings` と共用)。

## 類似ゲームの指定

- 議論の最初の投稿で「類似ゲーム「A」「B」」と書く (ペーパー調整の類似ゲーム指定と同じ書き方)。
- ペーパー確認中に「類似ゲーム「A」」と返信する (メカニクスの参考追加に加えて、A の声も集めて混ぜる)。

## 設定 (`flow.userVoices`)

| キー | 既定 | env |
|---|---|---|
| `enabled` | true | `DISCUTERE_FLOW_USER_VOICES_ENABLED` |
| `steamMaxReviews` | 300 | `DISCUTERE_FLOW_USER_VOICES_STEAM_MAX` |
| `steamLanguages` | `["japanese","english"]` | `DISCUTERE_FLOW_USER_VOICES_STEAM_LANGUAGES` (カンマ区切り) |
| `glabMaxImpressions` | 100 (1-200) | `DISCUTERE_FLOW_USER_VOICES_GLAB_MAX` |
| `voluptasBaseUrl` | Excubitor が配る `VOLUPTAS_URL` (旧綴り `VOLPUTAS_URL` も読む。それも無ければ空 = Voluptas からは集めない) | `DISCUTERE_VOLUPTAS_BASE_URL` (明示指定が優先) |

Voluptas の認証は Cernere service token (target `volputas`、scope `persona-export:read`)、
移行期間は `DISCUTERE_VOLUPTAS_EXPORT_TOKEN` の固定トークンにも落ちる。

## 正直な限界

- ゲームの解決は Steam のストア検索の候補名と議題の文字列一致に頼る。議題にゲーム名が無い
  (「ガチャ天井を下げるか」だけ) 場合は解決できない。「」でゲーム名を明示すると確実になる。
- 収集した声の検索 (均等混合) はスレッドのメモリにあり、Discutere の再起動で消える
  (再開時は既存のキーワード検索に戻る)。取り込んだ声とベクトルは KG に残る。
