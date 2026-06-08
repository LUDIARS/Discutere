# Crawler — External Discussion Sources (議論データ取り込み)

無料で取得できる **外部のゲーム議論データ** (ユーザー発話) を収集し、 Discatier Core
の `utterances` / `reactions` に **匿名化して取り込む** ためのモジュール。

`spec/crawler/DESIGN.md` (攻略 KG ビルダー) とは **目的が異なる**:

| | DESIGN.md (既存) | 本書 (EXTERNAL-SOURCES) |
|---|---|---|
| 収集対象 | 攻略情報 (事実・構造) | ユーザーの **生の議論・感想** |
| 取り込み先 | Game / Mechanic / Aesthetic ノード | **Utterance / Reaction** |
| 形態 | 要約 (原文転載しない) | 発話の**原文を保持** (匿名化・ToS 範囲内) |
| 用途 | クロスゲーム KG 参照 | persona-engine 学習 / designGap 種出し |

LUDIARS 短縮コード: **Di** / モジュールパス: `src/crawler/sources/`

## 1. 目的とスコープ

Discatier の議論エンジン (persona-engine / auto-discussion / designGap) は、 Discord
ギルド内の発話だけでは**コールドスタート**する。 そこで、 公開されているゲーム議論
(レビュー・コメント) を収集し、 `utterances` として投入して **議論の種・学習素材** にする。

- **入力**: ゲーム / 動画 / スレッド単位の「議論の場」
- **出力**: Discatier Core の `utterances` (+ 賛否は `reactions`)
- **同一性**: 投稿者の **公開・安定 ID** (SteamID 等) を speaker_id として保持し、
  同一人物の発話を横断同定して persona を積む。 ただし **個人特定情報は議論ユーザに
  見せず** 内部ペルソナ表示名でマスクする (原則「情報精度 > プライバシー」だが露出は遮断、§6)。
  workspace は `knowledge` 固定
- **非リアルタイム**: バッチ取り込み (日次 cron 想定)。 ライブ監視はしない

### 取得元 (優先順)

ユーザー合意の収集元。 **API のある所を主軸**、 スクレイピングは補助。

| # | 取得元 | 取得手段 | 認証 | コスト | Phase |
|---|---|---|---|---|---|
| 1 | **Steam レビュー** | 公開 `appreviews` エンドポイント | 不要 | 無料・無制限 | 1 |
| 2 | **YouTube コメント** | Data API v3 (2 クローラ: 動画 / コメント) | API key | 無料 (10k units/日) | 1 |
| 3 | **Reddit** | OAuth2 API | client id/secret | 無料 (~60 req/min) | 2 |
| 4 | **ニコニコ** | Snapshot Search + nvComment | 一部不要 | 無料 (rate 制限) | 2 |
| 5 | **Fandom** | MediaWiki API + Discussions API | 不要 | 無料・CC-BY-SA | 3 |

## 2. アーキテクチャ

すべての取得元は **共通の collector インターフェース** を実装し、 正規化済みの
`ExternalUtterance[]` を返す。 取得とインポートは **2 段** に分け、 中間 JSONL を挟む
(DESIGN.md の md round-trip と同じ思想 — 取得結果を人間/PR がレビューしてから DB 投入)。

```
                fetch 段 (collector)              import 段 (external-importer)
  ┌────────────┐   ExternalUtterance[]   ┌──────────────┐   utterance/reaction events
  │ source     │ ───────────────────────▶│  normalizer  │ ─────────────────────────────▶ Discatier Core
  │ collector  │                         │ (匿名化/dedup │                                 (utterances /
  │  (1 per    │                         │  /sentiment) │                                  reactions)
  │   source)  │           ┌─────────────┴──────────────┘
  └────────────┘           ▼
        │         data/external/<source>/<key>.jsonl   ← レビュー可能な中間形態
        │           (1 行 = 1 ExternalUtterance)
        ▼
   src/crawler/sources/<source>.ts
```

- **collector** (`src/crawler/sources/<source>.ts`): 取得元固有の API/HTML を叩き、
  `ExternalUtterance` に変換するだけ。 DB を知らない (SRP)。
- **normalizer** (`src/crawler/sources/normalize.ts`): 匿名化・dedup・(任意) sentiment
  付与。 全 source 共通。
- **external-importer** (`src/crawler/external-importer.ts`): JSONL → `core.repos.utterance.create`
  / `core.repos.reaction.create`。 既存 `importer.ts` (KG 用) とは別ファイル。
- **registry** (`src/crawler/sources/index.ts`): source 名 → collector の対応表。
  CLI / cron はここ経由でディスパッチ。

### 既存資産の再利用

- **sentiment** (`src/crawler/sentiment/`): 既存の lexicon ベース感情判定を流用し、
  賛否を `reactions` (reactionType=`positive`/`negative`, intensity=score) に変換。
- **md-format / importer**: パターンを踏襲 (frontmatter+本文 → 構造化)。 ただし
  外部発話は **JSONL** にする (件数が多く 1 ファイル 1 発話の md は非現実的)。

## 3. データモデル — `ExternalUtterance`

collector の出力かつ JSONL の 1 行。 `src/crawler/sources/types.ts` に定義する。

```ts
interface ExternalUtterance {
  source: "steam" | "youtube" | "reddit" | "niconico" | "fandom";
  nativeId: string;        // 取得元での一意 id (dedup key の素)
  gameSlug: string;        // どのゲームの議論か (Game.id と揃える / 例 "hollow-knight")
  threadKey: string;       // 議論の場の単位 → session_id に対応 (動画id/スレッドid/appid)
  content: string;         // 発話本文 (原文)
  lang?: string;           // "ja" | "en" など (判明すれば)
  postedAt: number;        // epoch ms
  authorId: string;        // ★公開・安定な同一性 ID (SteamID64 / YouTube channelId 等)。
                           //   同一人物の判定アンカー = persona の素 (§6)。保持する。
  authorName?: string;     // 公開表示名 (persona の手触り用。任意。逆引き用途には使わない)
  parentNativeId?: string; // 返信元 (スレッド構造) → responds_to に対応
  signal?: {               // 賛否・人気度 (あれば)
    votedUp?: boolean;     // Steam recommended / YT/Reddit は省略
    upvotes?: number;
  };
  sourceUrl: string;       // canonical な引用元 (監査・attribution 用)
}
```

### Discatier Core へのマッピング

`core.repos.utterance.create(...)` (= `UtteranceCreated` event) に落とす:

| ExternalUtterance | utterances カラム | 変換 |
|---|---|---|
| — | `workspace_id` | 固定 `"knowledge"` |
| `threadKey` | `session_id` | `ext:<source>:<threadKey>` (議論の場 = session) |
| `authorId` | `speaker_id` | `ext:<source>:<authorId>` (公開 ID をそのまま安定アンカー化 §6) |
| `content` | `raw_content` | 原文 |
| `postedAt` | `posted_at` | そのまま |
| `parentNativeId` | `responds_to` | 同バッチ内の nativeId→utteranceId 解決 |
| `signal.votedUp` | (reactions) | true→`positive` / false→`negative` |

- **session 作成**: `threadKey` ごとに `core.repos.session.create` を 1 回 (title=
  ゲーム名+source、 mode=`"external"`)。 既存 session があれば再利用 (upsert)。
- **responds_to**: collector は **親→子の順** で JSONL に並べる。 importer は
  `nativeId → 採番した utteranceId` の map を保持し、 子の `parentNativeId` を解決。
  解決できない親 (バッチ外) は `responds_to=null`。

### dedup

- key = `source + ":" + nativeId`。 取り込み済み key を `data/external/.ingested.sqlite`
  (単一テーブル `ingested(source, native_id, imported_at)`) に記録。
- importer は既取り込み key をスキップ → **再 fetch しても二重投入しない** (冪等)。
- ※ `utterances` 自体には外部 id 列を足さない (schema 不変) ため、 dedup は
  importer 側の sidecar DB で持つ。

### source / sourceUrl の保持 (露出の透明性用 / 2026-06-08)

§6 露出制御で **ソース種別 + 元 URL を end user に開示**するため、 取り込み時に
utterance ごとの出所を引けるようにする。 `utterances` schema は不変のまま、
**sidecar `source_attribution`** に保持する (`data/external/.attribution.sqlite`、
raw-store と同じ思想):

```sql
CREATE TABLE source_attribution (
  utterance_id TEXT PRIMARY KEY,
  source       TEXT NOT NULL,   -- "youtube" | "niconico" | ...
  source_url   TEXT NOT NULL,   -- canonical な元 URL (露出時のリンク先)
  native_id    TEXT NOT NULL,   -- 取得元一意 id (調整 UX の照合用)
  game_slug    TEXT,            -- どのゲームの議論か (データ調整 UX のフィルタ)
  imported_at  INTEGER NOT NULL
);
```

- external-importer が utterance を採番した直後に 1 行 upsert する (全 source。 短文
  コメントも含む。 raw-store は長文要約専用なので別物)。
- 露出 serializer は `speaker_id` をペルソナ名へマスクしつつ、 この sidecar から
  `source` / `source_url` を引いて出所バッジを付ける。
- データ調整 UX (§13) はこのテーブルを source / game_slug で集計・一覧する。

## 4. 取得元ごとの仕様

### 4.1 Steam レビュー (Phase 1 / 最優先)

- **エンドポイント**: `GET https://store.steampowered.com/appreviews/{appid}?json=1`
  - 主パラメータ: `num_per_page=100` / `cursor=*`(初回) / `filter=recent` /
    `language=japanese,english`(複数可) / `review_type=all` / `purchase_type=all` /
    `day_range=365`
  - ページング: レスポンスの `cursor` を URL エンコードして次リクエストへ。
    `reviews` が空 or 既出 cursor で停止。
- **APIキー不要**。 ToS 上 review 本文の**公開再配布はしない** (社内学習のみ / §7)。
- **appid 解決**: 入力はゲーム名。 `GET https://api.steampowered.com/ISteamApps/GetAppList/v2/`
  を 1 回取得しローカル cache → 名前一致で appid 特定。 曖昧なら config で明示。
- **マッピング**: `reviews[].recommendationid`→nativeId、 `review`→content、
  `timestamp_created*1000`→postedAt、 `author.steamid`(SteamID64)→**authorId**、
  `voted_up`→signal.votedUp、 `votes_up`→signal.upvotes、 threadKey=`appid`。
  - SteamID64 は安定・公開 ID。 同一人物の複数レビュー/別ゲーム発話を横断同定でき、
    persona の素になる。 author.num_games_owned / playtime 等の公開属性も persona
    補助に使える (任意)。
- **言語**: `language` で絞れるので日本語議論を集めやすい。

### 4.2 YouTube (Phase 1 / 動画クローラ + コメントクローラの 2 本)

API key (無料、 Google Cloud で発行) 必須。 **日次 quota 10,000 units** を共有予算
として `src/crawler/sources/youtube-quota.ts` で消費を加算し、 上限で停止。

#### (a) 動画クローラ `src/crawler/sources/youtube-videos.ts`

ゲーム名 → コメントを取りに行く候補 **動画 id リスト** を作る (discovery)。 出力は
work queue `data/external/youtube/videos/<gameSlug>.jsonl` (`{videoId,title,channelId,publishedAt}`)。

- **安い経路を優先** (quota 節約):
  - チャンネルが分かる場合: `channels.list(part=contentDetails)` で uploads playlist
    id を得て `playlistItems.list` (各 **1 unit**/100件) で動画列挙。
  - キーワード検索が必要な場合のみ `search.list` (**100 units**/call) を使い、
    **1 ゲームあたり呼び出し回数を config で上限化** (既定 1〜2 call)。
- 取得した動画の `statistics.commentCount` (`videos.list`, 1 unit/50件) で
  コメント無効/0 件を間引く。

#### (b) コメントクローラ `src/crawler/sources/youtube-comments.ts`

動画 id → コメント本文。

- `commentThreads.list(part=snippet,replies, maxResults=100, order=relevance|time)`
  (**1 unit**/call, 100 thread/page) でトップレベルコメント + 一部返信。
- 返信が `totalReplyCount` を超えて切れている場合のみ `comments.list(parentId=...)`
  (1 unit) で追加取得。
- **マッピング**: `comment.id`→nativeId、 `textOriginal`→content、
  `publishedAt`→postedAt、 `authorChannelId`(UCxxxx)→**authorId** /
  `authorDisplayName`→authorName、 返信は `parentNativeId`=トップコメント id、
  threadKey=`videoId`。 `likeCount`→signal.upvotes。
  - channelId は安定・公開 ID。 同一チャンネル主の全コメントを横断同定でき persona 化可。
- コメント無効動画は `commentsDisabled` エラーを握って skip。

#### quota 設計

- `commentThreads.list` は 1 call=1 unit=100 コメント → 10k units で理論 **100 万
  コメント/日**。 ボトルネックは discovery 側の `search.list` (100 units)。
- 予算配分の既定: discovery に最大 2,000 units、 残りをコメント取得に。 config で可変。

#### (c) 「プレイ動画 → 人気順 → 関係コメント」 パイプライン設計 (2026-06-04 追加)

ゲームタイトルから **実際のプレイ動画を人気順に並べ、 上位動画のコメントから
ゲームに関係する意見だけを取り込む** ための end-to-end フロー。 「感情値 (sentiment)
が多くなる」 のは織り込み済み (= プレイ動画コメントは感想・感情表現が主)。

1. **discovery (人気順)**: `discoverVideosBySearch({ query, order: "viewCount" })`。
   - query は `<ゲーム名> + 実況|プレイ|gameplay|playthrough` を OR で付与し、 攻略でも
     レビューでもない **プレイ動画** に寄せる (config `youtube.playVideoTerms`)。
   - `order=viewCount` で視聴回数降順 = 人気順。
2. **動画メタ enrich + 厳密ランク付け**: 候補 videoId を 50 件束で
   `videos.list(part=statistics,snippet,contentDetails)` (1 unit/50 件) して
   `viewCount / likeCount / commentCount / duration / categoryId` を取得。
   - `categoryId=20`(Gaming) 以外、 `commentCount=0`、 極端に短い (Shorts) 動画を間引く。
   - `viewCount` 実数で再ソート (search の順位は厳密でないため確定ランクを付け直す)。
3. **上位 N 動画のコメント取得**: 上位から `youtube-comments` を回す (N は config
   `youtube.topVideos`、 既定 10)。 既存 quota 予算内で打ち切り、 落とした分は log。
4. **ゲーム関係フィルタ (取り込み前)**: コメントを「その**ゲームに関係する意見**か」 で
   選別してから ingest する。 二段:
   - **安価な事前フィルタ**: 雑コメント (絵文字のみ / "草" 単独 / タイムスタンプ "12:34" /
     投稿者宛ノイズ / URL のみ) と極端な短文を正規表現で除外。
   - **関連度判定**: ゲーム名・主要メカニクス語 (KG の mechanics/aesthetics から生成した
     辞書) を含むか + 任意で LLM 一括バッチ判定 (`relevant: bool`)。 LLM 判定は quota とは
     別予算なので config `youtube.useLlmRelevance` で on/off。
   - 関係しないコメントは ingest しない (= KG を実況者個人の雑談で薄めない)。
5. **取り込み**: 残ったコメントを既存 importer で `ext:youtube:<commentId>` 化。
   `likeCount`→signal.upvotes、 動画の人気度は session(=videoId) のメタとして保持。

#### (d) YouTube API から追加で取得するデータ

「他に取れるデータがあれば取る」 → 議論の素・重み付けに使えるものを併取する:

| データ | API (part) | 用途 |
| --- | --- | --- |
| 動画 統計 (view/like/comment 数) | `videos.list(statistics)` | 人気順ランク + 意見の母集団規模の重み |
| 動画 説明文 / タグ | `videos.list(snippet)` | 議論の種 (どんな切り口の動画か) の文脈 |
| 動画 カテゴリ / 長さ | `videos.list(snippet,contentDetails)` | Gaming 判定・Shorts 間引き |
| チャンネル情報 (登録者数) | `channels.list(statistics)` | 投稿者 persona の影響力の重み |
| コメントの like 数 / 返信数 | `commentThreads.list` | 意見スコア (signal.upvotes) |
| 字幕 (取得可能な場合) | `captions.list`/timedtext | 動画本体の主張を議論の種にする (任意・Phase 後段) |

> ToS 注意: 字幕の機械取得は動画によって不可・グレーなので **任意かつ後段**。 取得不可は
> 握って skip。 個人特定はしない (channelId は公開アンカーとしてのみ利用、 §6 の露出制御に従う)。

### 4.3 Reddit (実装済 / `src/crawler/sources/reddit.ts` 2026-06-05)

> 実装: client_credentials grant で app-only token → 検索 → 上位スレッド → コメントツリー平坦化。
> CLI `crawl.ts ext-ingest reddit <gameSlug> --q "<query>" [--sub <subreddit>] [--threads N]`。
> 認証は env `DISCUTERE_REDDIT_CLIENT_ID` / `DISCUTERE_REDDIT_CLIENT_SECRET` / `DISCUTERE_REDDIT_USER_AGENT`
> (config に平文を置かない)。 クロールチャンネルに reddit スレッド URL を貼ると当該スレッドのコメントを取込。

- **OAuth2** (`script` app, client_credentials または password grant) で
  `POST https://www.reddit.com/api/v1/access_token` → bearer。
- **取得**: `GET https://oauth.reddit.com/r/{sub}/search?q=...&restrict_sr=1&sort=top&t=year`
  でスレッド列挙 → 各スレッド `GET /comments/{id}?depth=10&limit=500` でコメントツリー。
- **rate**: OAuth で ~60 req/min。 `User-Agent` 必須 (`LUDIARS-Discutere/<ver> by /u/<acct>`)。
- **マッピング**: comment `name`(t1_xxx)→nativeId、 `body`→content、
  `created_utc*1000`→postedAt、 `author`(username)→**authorId**(=authorName)、
  `parent_id`→parentNativeId、 threadKey=submission id、 `ups`→signal.upvotes。
  - Reddit username は公開・安定。 同一ユーザの議論を横断同定でき persona 化可。
- `[deleted]` / `[removed]` は skip。 削除尊重ポリシー (§7)。

### 4.4 ニコニコ (Phase 2)

- **動画 discovery**: Snapshot Search API v2 (key 不要)
  `GET https://snapshot.search.nicovideo.jp/api/v2/snapshot/video/contents/search`
  `?q=<ゲーム名>&targets=title,description,tags&fields=contentId,title,viewCounter&_sort=-viewCounter`
  → contentId 列挙。
- **コメント**: watch ページ経由でしか threadKey を取れない。
  `GET https://www.nicovideo.jp/api/watch/v3_guest/<contentId>` 相当で `nvComment`
  の `server` / `threadKey` / `params` を取得 → `POST https://nvcomment.nicovideo.jp/v1/threads`。
  **2 段必要・仕様変動リスクが高い** ため Phase 2 の中でも後段。
- **マッピング**: comment `id`→nativeId、 `body`→content、 `postedAt`→postedAt、
  `userId`(公開コメント userId)→**authorId**、 threadKey=`contentId`。
  - ニコニコ userId は数値の安定 ID (実名ではない) で公開。 同一 userId の弾幕を
    横断同定でき persona 化可。 184(匿名コメント)は authorId 無し → persona 対象外。
- ニコニコのコメントは時系列の弾幕で**返信構造を持たない** → `responds_to=null` 固定。

### 4.5 Fandom (Phase 3)

- **MediaWiki API**: `GET https://<wiki>.fandom.com/api.php?action=query&format=json&...`
- 議論データの所在は wiki によって異なる:
  - **Talk ページ**: `prop=revisions&rvprop=content` でノートページ本文 → 節ごとに分割。
  - **Discussions** (Fandom の掲示板機能): `GET https://services.fandom.com/discussion/{siteId}/posts`。
    siteId は `?action=query&meta=siteinfo` から解決。
- **ライセンス**: Fandom 本文は **CC-BY-SA**。 `sourceUrl` + attribution を必ず残す (§7)。
- **マッピング**: post id→nativeId、 本文→content、 created→postedAt、 親 post→
  parentNativeId、 threadKey=スレッド/記事 id、 投稿者 userId/userName→authorId/authorName。

### 4.6 Webサイト (任意 URL の記事 / Phase 1 / 2026-06-04 追加)

レビュー・考察ブログ・ニュース記事など **任意の公開 URL** を 1 件取得し、 本文を抽出して
**1 記事 = ひとりの論者の 1 意見** として取り込む。 API キー不要 (公開 HTML を直接取得)。
`src/crawler/sources/website.ts`。

- **取得**: `fetchWebsiteArticles({ urls[], gameSlug })`。 URL ごとに `fetch` →
  `extractArticle(html)` で `{title, text, author}` を抽出。 失敗 URL (404 / 本文 < 80 字
  = 非記事) は skip して続行。 同一ドメイン連続取得は politeDelay (既定 1500ms)。
- **本文抽出 (依存ライブラリ無しの軽量ヒューリスティック)**: `<script>/<style>/<nav>/
  <header>/<footer>/<aside>` を除去 → `<article>` → `<main>` → `<body>` の順で本文領域を
  選択 → タグ除去 + HTML エンティティ復号 + 空白圧縮。 上限 8,000 字。 完全な readability
  ではないが「粗く動かす」段階には十分 (将来 Lector に寄せる余地、 §11)。
- **マッピング**: `normalizeUrl(url)`(hash + utm/fbclid 等の tracking 除去)→ nativeId /
  threadKey (記事 1 本 = 1 スレッド = 1 session)、 `title + 本文`→content、
  `<meta author>` があれば `host:著者`、 無ければ `host` を **authorId** (公開・安定な
  サイトアンカー §6)、 日本語含有で `lang=ja` 推定、 取得時刻→postedAt、 正規化 URL→sourceUrl。
- **dedup**: `(website, 正規化 URL)` で sidecar 冪等。 同一記事の再取り込みは skip。
- **要約 / raw の 2 層化 (id=67 / 2026-06-05 追加)**: website は長文なので、 取り込み時に
  LLM で要約を生成し (`summarize.ts`)、 **utterances 本文 (= 議論コンテキストで参照される側)
  には要約を入れてトークンを節約**、 **raw 全文は raw-store (`source_raw` sidecar、
  `data/external/.raw.sqlite`) に utterance id 紐付けで退避**する。 必要時のみ raw を引く。
  - `importExternalUtterances(..., { rawStore })` + `item.summary` がある時に発動。
  - LLM 未設定 / 短文 (< 600 字) は従来通り raw を本文に (要約しない)。 Steam/YouTube/Reddit
    のコメントは短文なので 2 層化対象外 (website 限定)。
  - CLI `ext-ingest website` は config の LLM があれば自動で要約。 クロールチャンネルも同様
    (gateway が要約器を注入)。
- **ToS**: robots / レート制限を尊重 (§7)。 ペイウォール内本文・ログイン必須ページは取得しない。
  著作権配慮で **本文は議論の素として内部利用**、 sourceUrl を必ず保持し原文へ辿れるようにする。

> 補足: 「ワンダと巨像」 のように **Steam 非掲載 (PS 専売・PC 版なし)** のタイトルは
> Steam レビューが存在しないため、 Web 記事 (この source) と YouTube を主取得元にする
> (2026-06-04 ユーザ決定)。

## 5. 設定 (config)

`src/config.ts` の typed config に `externalSources` を追加 (`discutere.config.json`)。

```jsonc
{
  "externalSources": {
    "enabled": false,                    // 既定 OFF (opt-in)
    "rateLimitPerDomainMs": 5000,        // 同一ドメイン下限間隔 (DESIGN.md 準拠)
    "userAgent": "LUDIARS-Discutere-Crawler/0.1 (+contact)",
    "games": [                           // 収集対象。 source 別 id を明示
      { "slug": "hollow-knight", "name": "Hollow Knight",
        "steamAppId": 367520, "youtube": { "keywords": ["Hollow Knight 考察"] },
        "subreddits": ["HollowKnight"], "fandomWiki": "hollowknight" }
    ],
    "youtube": { "dailyQuota": 10000, "discoveryQuotaCap": 2000, "order": "relevance" },
    "steam":   { "languages": ["japanese", "english"], "dayRange": 365 },
    "reddit":  { "sort": "top", "timeRange": "year" }
  }
}
```

### シークレットは config に置かない

[[feedback_config_and_secrets]] / [[feedback_secret_per_user_memory_only]] に従い、
**鍵類は平文 config に書かない**:

- `youtube.apiKey` / `reddit.clientId` / `reddit.clientSecret`
  → **env or Infisical 経由** で起動時取得 (`externalSources` には非シークレットのみ)。
- env 名: `DISCUTERE_YOUTUBE_API_KEY` / `DISCUTERE_REDDIT_CLIENT_ID` /
  `DISCUTERE_REDDIT_CLIENT_SECRET`。

## 6. 話者の同一性 (persona アンカー)

> **方針 (2026-06-04 ユーザー決定)**: hash 匿名化は議論主の **persona 情報を壊す** ため
> 採らない。 各プラットフォームが公開している **安定な同一性 ID** (SteamID64 /
> YouTube channelId / Reddit username / niconico userId 等) を **そのまま speaker_id の
> アンカーとして保持** し、 「同じ人物の発話」 を横断同定して persona を積み上げる。
>
> **統治原則: 情報精度 > プライバシー**。 persona の精度を最優先し、 同定情報は捨てない。
> ただしそれは **保管・内部解析レイヤーに限る**。
>
> **露出方針 (2026-06-08 改訂 — データソースの透明性を上げる)**: 発話の **出所
> (ソース種別 + 元 URL)** は end user にも **開示する** (どこの誰がどう言ったかを
> 辿れる透明性 > 出所の秘匿)。 一方、 **個人アンカー (公開 ID = `authorId` /
> 表示名 = `authorName`) は引き続きマスク**して内部ペルソナ表示名で見せる。
> = 「**出所は透明 / 個人は仮名**」 の二層。 旧方針 (ソース URL も全マスク) は撤回。

### なぜ保持するか
- persona-engine は「**誰が**どんな論調で何を語るか」 を学習する。 同一性 ID が無いと
  全発話が無名の独立点になり、 論者の一貫性・嗜好・語り口 (= persona) が消える。
- 対象はいずれも **公開かつ仮名の platform ID**。 実名・連絡先ではない。

### 保持するもの / しないもの
- **保持**: `authorId` (公開・安定 ID。 `speaker_id = ext:<source>:<authorId>`) +
  `authorName` (公開表示名。 persona の手触り)。 公開属性 (Steam 所持本数/プレイ時間 等) も任意で persona 補助。
- **しない**: 実名・メール・IP・platform 外の同定情報の取得や推定。
  authorId → 実世界個人への逆引き / プロファイリングはしない (§10 非ゴール)。
- **プラットフォーム横断の同一人物紐付けはしない**: SteamID と YouTube channelId は
  別人として扱う (確実な対応付けが不可能なため)。 persona は **platform-identity 単位**。

### 露出制御 (出所は透明 / 個人は仮名)

露出面 (Discord 返信・要約・dashboard・API 等 end user が見る面) では、 **出所メタは
開示し、 個人アンカーはマスクする**:

| フィールド | 露出レイヤー (end user) | 根拠 |
|---|---|---|
| `source` (種別: youtube/niconico/steam/…) | **開示** | データソースの透明性 |
| `sourceUrl` (元 URL) | **開示** (原文へのリンク = attribution) | 同上。原文転載ではないので §7 とも整合 |
| `authorName` (公開表示名) | **マスク** (内部ペルソナ表示名へ置換) | 個人アンカーは仮名化 |
| `authorId` / `speaker_id` (公開 ID) | **マスク** (素通し禁止) | 同上。逆引き防止 |

- **保管レイヤー (DB / 内部解析)**: `authorId` / `authorName` をフル精度で保持。
  persona-engine の学習・同定はここで完結する (変更なし)。
- **露出レイヤー**: 発話を見せる時は `論者#a1b2` 等の **不可逆な内部ペルソナ表示名** +
  **出所バッジ** (`[YouTube ↗]` など、 リンク先=`sourceUrl`) をセットで提示する。
  「誰が」 は仮名のまま「どこ発か」 は辿れる。
  - 露出名 → 公開 ID への逆引きは end user 側からは不可能 (対応表は内部のみ)。
- **実装方針**: utterance に紐づく `speaker_id` (= 公開 ID アンカー) は **API レスポンス /
  Discord 出力に素通ししない** → 表示用 persona ラベルへ解決してから出す。 一方
  `source` / `sourceUrl` は出力に **載せる** (sidecar `source_attribution` から projection、 §3)。
  presentation 層で「個人だけマスク・出所は透過」の serializer を一本に集約する。
- **admin/運用面**: 生の `authorId` / `authorName` の参照は admin (admin-id allowlist) のみ。

### 既存「個人データ禁止」ルールとの関係
- CLAUDE.md「個人データ」/ [[project_personal_data_rule]] は **LUDIARS 利用者の個人データ**
  (Cernere 単一情報源) を対象としたもの。 本件は **外部公開の仮名 platform ID** であり、
  Cernere 管理対象でも LUDIARS ユーザでもない。 両者は別レイヤーとして切り分ける。
- ただし運用境界は守る: workspace は `knowledge` (Discord guild 由来の内部発話とは
  混ぜない)、 原文の外部再配布はしない (§7)、 削除された発話は取り込まない (§7)。
- → このルール整合は **CLAUDE.md §個人データ に追記して明文化する** (本 PR の TODO)。

## 7. ToS / 著作権 / レート制限 (= 必ず守る範囲)

DESIGN.md §「制約」を継承しつつ、 外部発話の特性を追加する。

- **rate limit**: 同一ドメイン `rateLimitPerDomainMs` (既定 5s) を下回らない。
  API の公式 quota (YouTube 10k/日, Reddit 60/min) を上限として遵守。
- **User-Agent 明示**: 全リクエストに連絡先付き UA。
- **再配布しない**: 収集した発話原文は **Discatier 内部の学習・議論種** に閉じる。
  LUDIARS Pages / 外部 API / Discord への**原文転載はしない** (要約・統計は可)。
- **削除の尊重**: Reddit `[deleted]` / YouTube 削除コメントは取り込まない。 再 fetch 時に
  元が消えていれば再投入もしない (取得時点スナップショット)。
- **ライセンス**: Fandom = CC-BY-SA を `sourceUrl` で記録。 Steam/YouTube/Reddit/ニコニコは
  各 API ToS の非商用・内部利用範囲。 商用転用が必要になったら都度ポリシー再確認。
- **robots.txt**: スクレイピング経路 (ニコニコ watch ページ等) は robots を尊重。

## 8. CLI

`scripts/crawl.ts` にサブコマンドを追加 (or 新規 `scripts/external-crawl.ts`)。

```sh
# 1 source × 1 ゲームを fetch → 中間 JSONL
npx tsx scripts/crawl.ts ext-fetch steam hollow-knight
npx tsx scripts/crawl.ts ext-fetch youtube hollow-knight        # 動画→コメントを連続実行
npx tsx scripts/crawl.ts ext-fetch youtube-videos hollow-knight # 動画クローラ単体
npx tsx scripts/crawl.ts ext-fetch youtube-comments <videoId>   # コメントクローラ単体

# JSONL → Discatier Core (同一性アンカー付与 + dedup + sentiment)
npx tsx scripts/crawl.ts ext-import data/external/steam/hollow-knight.jsonl

# 全 source × config.games を一括 (cron 用)
npx tsx scripts/crawl.ts ext-run-all
```

## 9. Phase ロードマップ

### Phase 0 (本 PR) — 設計のみ
- 本書 + `ExternalUtterance` 型 + JSONL フォーマット確定
- collector / normalizer / external-importer の **interface 定義のみ** (実装なし)
- config schema (`externalSources`) のドラフト

### Phase 1 — Steam + YouTube
- Steam collector (公開エンドポイント、 最も簡単・高品質)
- YouTube 動画クローラ + コメントクローラ + quota 管理
- normalizer (匿名化・dedup・sentiment) + external-importer + CLI `ext-fetch`/`ext-import`
- sample 1 ゲーム (Hollow Knight) で round-trip テスト

### Phase 2 — Reddit + ニコニコ
- Reddit OAuth collector
- ニコニコ Snapshot Search + nvComment collector (2 段取得)

### Phase 3 — Fandom + 自動化
- Fandom MediaWiki / Discussions collector
- 日次 cron (`ext-run-all`) → 取り込み → designGap 種出し
- 既存 auto-discussion / persona-engine 学習との結線 (外部発話を学習素材に)

### Phase 4 — 結合
- 取り込んだ外部議論を `src/core/bridge/gap/detector.ts` に流し designGap 化
- sentiment による hotspot ランキング (`src/core/cache/hotspot-rank.ts`) へ接続

## 10. 非ゴール
- ライブ/リアルタイム監視 (バッチのみ)
- **原文 (本文) の外部公開・再配布** — 出所メタ (source 種別 + `sourceUrl` リンク) の開示は
  attribution であり可 (§6 改訂)。 ただし発話 **本文そのもの** を LUDIARS Pages / 外部 API へ
  再掲載することは引き続きしない (§7)。
- **公開 platform ID → 実世界個人への逆引き / 名寄せ / プロファイリング** (公開仮名 ID を
  persona アンカーとして保持はするが、 実名特定はしない)。 `authorId` / `authorName` は
  露出面でマスク維持 (§6)。
- **プラットフォーム横断の同一人物紐付け** (Steam↔YouTube↔Reddit を同一人物と推定しない)
- X(Twitter) / Discord 他サーバ / 5ch (ToS・コスト・脆さで今回は対象外)
- `utterances` schema 変更 (既存カラムに収める。 dedup / source 帰属 / KG 選択は sidecar・config 側)
- **KG ランタイム・ホットスワップ** (無停止での KG 貼り替え) は今回非対応 — KG 切替は
  config 選択 + 再起動 (§12)。 ホットスワップは follow-up。

## 11. 参照
- 既存攻略 KG: `spec/crawler/DESIGN.md`
- Utterance 取り込み API: `src/core/repositories/base.ts` (`createUtteranceRepo`)
- 発話 schema: `src/core/db/schema.ts` (`utterances` / `reactions`)
- 既存 sentiment: `src/crawler/sentiment/`
- Discord 自然取り込み (内部発話の前例): `src/discord-hook/auto-discussion.ts`

## 12. タスク別 KG (複数 KG 宣言 + 起動時選択 / 2026-06-08)

学習データ KG を **タスク (収集目的) ごとに分割**し、 社内で運用を切り替えられるようにする。
これまでは `discatier.kuzuPath` 単一に集約していたが、 ゲーム/テーマ別にデータを隔離して
**調整しやすくする** ため複数 KG を宣言可能にする (データ調整 UX の前提)。

> **方針 (C / 2026-06-08 ユーザー決定)**: spec はタスク別 KG (複数宣言) を正式化する。
> 実装は **config でレジストリ宣言 + active を指定し、 切替は再起動**で行う (起動時選択)。
> 無停止ホットスワップは非対応 (§10、 follow-up)。

### config モデル — `discatier.knowledgeGraphs[]` + `activeKg`

```jsonc
{
  "discatier": {
    "activeKg": "default",                 // 起動時に開く KG の id (未設定なら "default")
    "knowledgeGraphs": [                    // タスク別 KG レジストリ
      { "id": "default",  "label": "総合",          "kuzuPath": "./data/kg/default.kuzu" },
      { "id": "hollow",   "label": "Hollow Knight",  "kuzuPath": "./data/kg/hollow.kuzu" },
      { "id": "trend2026","label": "2026トレンド検証", "kuzuPath": "./data/kg/trend2026.kuzu" }
    ]
    // 後方互換: 旧 `kuzuPath` 単独指定は id="default" の 1 KG として吸収する。
  }
}
```

- **解決順**: `knowledgeGraphs` があれば `activeKg` (既定 `"default"`) の `kuzuPath` を採用。
  無ければ旧 `kuzuPath` (それも無ければ adapter 既定) に倒す (完全後方互換)。
- env `DISCATIER_ACTIVE_KG` で active を上書き可 (再起動で反映)。
- **Core を開く全箇所が active KG を見る**: `createCore` 呼び出し / `queue/snapshot` /
  `analysis/gap-rate` / `crawler/quarantine` / `backup/runner`。 散在する `kuzuPath()` /
  `config.discatier.kuzuPath` 参照を **`resolveActiveKgPath(config)` 1 関数**に集約する (SRP)。
- sidecar (`.ingested` / `.raw` / `.attribution`) は KG ごとに分けるか共有かを選べるよう、
  既定は **active KG のディレクトリ配下**に置く (`<kgDir>/.ingested.sqlite` 等)。 KG を切り替えれば
  dedup 履歴・source 帰属も一緒に切り替わる (データ隔離が成立)。
- persona-engine db (`personaEngine.dbPath`) は KG に追従しない (発火制御は KG 非依存)。

### 切替の運用

- 切替 = config の `activeKg` 変更 (or `DISCATIER_ACTIVE_KG`) → **再起動**。
- データ調整 UX (§13) は現在の active KG と宣言済み KG 一覧を **表示**し、 「次に開く KG」 を
  config へ書き戻す (実切替は再起動)。 無停止切替は行わない。

## 13. データ調整 UX (admin / 2026-06-08)

「タスク別に KG を分けて運用切替する」 前提では、 **取り込んだデータを見て・絞って・除外できる
UX が運用価値の中心**になる。 admin dashboard に「データソース」面を新設する。

- **エンドポイント** (`src/api/datasource-routes.ts`、 全て `requireAdmin`):
  - `GET /api/admin/datasources` — active KG + 宣言済み KG 一覧、 source 別件数
    (`source_attribution` を source / game_slug で集計)、 直近取り込み時刻。
  - `GET /api/admin/datasources/:source/utterances?gameSlug=&limit=` — 当該 source の
    取り込み発話を一覧 (本文抜粋 + 出所 URL + ペルソナ名。 **生 ID は admin のみ可視**)。
  - `POST /api/admin/datasources/exclude` — 発話単位 or (source[, gameSlug]) 単位で
    取り込みを **除外**する。 除外は `dismissed` 相当 (学習・議論から外す。 [[既存 noise/quarantine 機構]]
    に倣う) + `.ingested` に「除外済み」 を残して **再取り込みを防ぐ**。
  - `POST /api/admin/datasources/active-kg` — 次に開く KG (`activeKg`) を config に書き戻す
    (再起動で反映。 即時切替はしない旨を返す)。
- **dashboard カード**: source 別件数バッジ + KG セレクタ (現在の active を強調) +
  各 source の「中身を見る」→ 発話一覧 (出所バッジ付き) + 行ごと「除外」ボタン。
- **既存資産の再利用**: 除外は `src/crawler/quarantine.ts` / noise 除去 (`noise-routes.ts`) の
  仕組みに寄せ、 二重実装しない。 一覧の発話マスクは §6 の露出 serializer を通す
  (個人マスク・出所透過)。
- スコープ: 閲覧 + 除外 + active KG 指定まで。 KG 間のデータ移動・マージ・編集 (書き換え) は
  今回非対応 (follow-up)。

## 14. AI が外部の声を引用する (RAG / 2026-06-08)

取り込んだ外部の声を **AI (persona-engine) が議論中に引用・参照して喋れる**ようにする。
FT (ファインチューニング) は Claude API / CLI 経路に存在しないため不採用 → **RAG (関連データを
ピックして prompt に注入)** を採る (ユーザ決定 2026-06-08、 選択肢 A)。

### 背景: なぜ間接参照だけでは足りなかったか

従来、 外部発話は `utterances` に取り込まれ **designGap / affect シグナル経由で間接的に**しか
AI に効かなかった (`prompt-builder` が prompt に載せるのは仮説 + 直近 gap + 現 session の発話
12 件のみ)。 = AI は「実際の声」を逐語で見ておらず、 出所も意識していなかった。

### retrieval (選択肢 A — キーワード一致 + 既存スコア順)

`context-provider.listRelevantExternalVoices(workspaceId, terms, limit)` を追加
(`discatier-engine-adapter` が実装):

1. 候補 = active KG の外部取り込み発話 (`speaker_id LIKE 'ext:%'`) を直近から最大 300 件。
2. スコア = (議題語ヒット数 × 10) + opinion-score (既存 `getOpinionScore`、 合意/支持の重み)。
   - 議題語 = `prompt-builder.extractTopicTerms(primaryGap)` (gap title + description を
     記号区切り + タイトル全体、 2 文字以上)。 日本語は substring 一致。
3. スコア 0 (関連語ヒットも支持も無い) は除外し、 議論を薄めない。 noise 除外
   ([[utterance_exclusions]]) 済みも除く。 上位 `limit` (既定 6) を返す。

### prompt 注入 + 露出 (§6 準拠)

- `prompt-builder` が `ctx.externalVoices` に積み、 「## 外部の声」 指示で
  「`○○ (source) はこう言ってる` と**出所を添えて引用・参照してよい**」と persona に促す。
- **個人はペルソナ名へマスク (`論者#xxxx`)、 出所 (source 種別 + sourceUrl) は開示** —
  §6「出所は透明 / 個人は仮名」と一致。 公開 ID (authorId) は prompt に出さない。
- 本文は 1 件 200 字上限・最大 6 件でトークン節約。 retrieval 未実装 / 該当 0 なら従来挙動
  (外部の声は載らない)。

### 非ゴール / follow-up
- **FT (ファインチューニング)**: Claude の第一者 API / CLI に学習エンドポイントが無いため不可。
- **embedding 類似度検索**: embedding 基盤が無いため今回非対応 (選択肢 C、 将来)。
- **LLM 関連度判定** (選択肢 B): 精度向上の余地。 まず A で運用して物足りなければ追加。
- topK / 有効化の config 化 (現状 `EXTERNAL_VOICE_TOP_K=6` 定数)。
