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
- **匿名**: 投稿者名・アカウントは保存しない (§6)。 workspace は `knowledge` 固定
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
  content: string;         // 発話本文 (原文。 匿名化は normalizer が行う)
  lang?: string;           // "ja" | "en" など (判明すれば)
  postedAt: number;        // epoch ms
  authorRaw?: string;      // 取得元の投稿者 handle (★normalizer で hash 化し破棄)
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
| `authorRaw` | `speaker_id` | `ext:<source>:<sha256(salt+authorRaw)[:16]>` (§6) |
| `content` | `raw_content` | 原文 (匿名化後) |
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
  `timestamp_created*1000`→postedAt、 `author.steamid`→authorRaw、
  `voted_up`→signal.votedUp、 `votes_up`→signal.upvotes、 threadKey=`appid`。
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
  `publishedAt`→postedAt、 `authorChannelId`→authorRaw、 返信は
  `parentNativeId`=トップコメント id、 threadKey=`videoId`。
  `likeCount`→signal.upvotes。
- コメント無効動画は `commentsDisabled` エラーを握って skip。

#### quota 設計

- `commentThreads.list` は 1 call=1 unit=100 コメント → 10k units で理論 **100 万
  コメント/日**。 ボトルネックは discovery 側の `search.list` (100 units)。
- 予算配分の既定: discovery に最大 2,000 units、 残りをコメント取得に。 config で可変。

### 4.3 Reddit (Phase 2)

- **OAuth2** (`script` app, client_credentials または password grant) で
  `POST https://www.reddit.com/api/v1/access_token` → bearer。
- **取得**: `GET https://oauth.reddit.com/r/{sub}/search?q=...&restrict_sr=1&sort=top&t=year`
  でスレッド列挙 → 各スレッド `GET /comments/{id}?depth=10&limit=500` でコメントツリー。
- **rate**: OAuth で ~60 req/min。 `User-Agent` 必須 (`LUDIARS-Discutere/<ver> by /u/<acct>`)。
- **マッピング**: comment `name`(t1_xxx)→nativeId、 `body`→content、
  `created_utc*1000`→postedAt、 `author`→authorRaw、 `parent_id`→parentNativeId、
  threadKey=submission id、 `ups`→signal.upvotes。
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
  `userId` は匿名 id だがさらに hash、 threadKey=`contentId`。
- ニコニコのコメントは時系列の弾幕で**返信構造を持たない** → `responds_to=null` 固定。

### 4.5 Fandom (Phase 3)

- **MediaWiki API**: `GET https://<wiki>.fandom.com/api.php?action=query&format=json&...`
- 議論データの所在は wiki によって異なる:
  - **Talk ページ**: `prop=revisions&rvprop=content` でノートページ本文 → 節ごとに分割。
  - **Discussions** (Fandom の掲示板機能): `GET https://services.fandom.com/discussion/{siteId}/posts`。
    siteId は `?action=query&meta=siteinfo` から解決。
- **ライセンス**: Fandom 本文は **CC-BY-SA**。 `sourceUrl` + attribution を必ず残す (§7)。
- **マッピング**: post id→nativeId、 本文→content、 created→postedAt、 親 post→
  parentNativeId、 threadKey=スレッド/記事 id。

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

- `youtube.apiKey` / `reddit.clientId` / `reddit.clientSecret` / 匿名化 `salt`
  → **env or Infisical 経由** で起動時取得 (`externalSources` には非シークレットのみ)。
- env 名: `DISCUTERE_YOUTUBE_API_KEY` / `DISCUTERE_REDDIT_CLIENT_ID` /
  `DISCUTERE_REDDIT_CLIENT_SECRET` / `DISCUTERE_EXTERNAL_ANON_SALT`。

## 6. 匿名化 (個人データ禁止)

CLAUDE.md「個人データ」/ [[project_personal_data_rule]] に従い、 **投稿者の同定情報を
一切保存しない**:

- `authorRaw` は normalizer で `speaker_id = "ext:<source>:" + sha256(salt + authorRaw).slice(0,16)`
  に変換し、 **元の handle/userId は JSONL にも DB にも残さない** (fetch 直後にメモリ上で hash)。
  - salt は秘匿 (env)。 これにより「同一人物の連投」「返信関係」は保てるが、
    handle への逆引きは不可。
- content 内に現れる @mention / 固有名 (実名らしき列) は **そのまま** (発話の一部)
  だが、 将来 PII マスキングを normalizer に足せる余地を残す。
- workspace は `knowledge` (匿名スペース) 固定。 guild 別 session には混ぜない。

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

# JSONL → Discatier Core (匿名化 + dedup + sentiment)
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
- 原文の外部公開・再配布
- 投稿者の同定・プロファイリング (匿名 hash のみ)
- X(Twitter) / Discord 他サーバ / 5ch (ToS・コスト・脆さで今回は対象外)
- `utterances` schema 変更 (既存カラムに収める。 dedup は sidecar DB)

## 11. 参照
- 既存攻略 KG: `spec/crawler/DESIGN.md`
- Utterance 取り込み API: `src/core/repositories/base.ts` (`createUtteranceRepo`)
- 発話 schema: `src/core/db/schema.ts` (`utterances` / `reactions`)
- 既存 sentiment: `src/crawler/sentiment/`
- Discord 自然取り込み (内部発話の前例): `src/discord-hook/auto-discussion.ts`
