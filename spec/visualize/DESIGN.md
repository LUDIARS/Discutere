# Visualize — Magic-link md exporter (Phase 0)

Discatier Core (議論エンジン) の hypothesis / gap / mechanic / aesthetic /
utterance / session を **1 ノード = 1 markdown ファイル** に書き出し、 ノード間
参照を `[[<type>:<id>]]` 形式のマジックリンクで繋ぐ。 viewer (Memoria /
GitHub markdown / 専用 renderer) が wikilink を解釈して相互ジャンプ可能。

LUDIARS 短縮コード: **Di** / モジュールパス: `src/visualize/`

## 設計の動機

- 議論エンジンの判断 (hypothesis 提案 / gap 検出 / mechanic 派生) のソースが
  追跡できない問題への解
- crawler の KG md (`data/games/<slug>.md`) と同じ「md = 永続化 + PR レビュー +
  人間閲覧」 文化を再利用
- frontend なしで Discord-only pivot と整合
- LUDIARS 全体 (Memoria / Corpus / Actio) が同じ wikilink 規約を採用すれば、
  クロスサービスで相互参照可能

## マジックリンク規約

形式: `[[<type>:<id>]]`

| Prefix | 対象 | 例 |
|--------|------|-----|
| `utt`  | utterance (発話)         | `[[utt:550e8400-e29b-41d4-a716-446655440000]]` |
| `hyp`  | hypothesis (仮説)        | `[[hyp:abc123]]` |
| `gap`  | design gap (設計ギャップ) | `[[gap:def456]]` |
| `mch`  | mechanic (メカニクス)     | `[[mch:ghi789]]` |
| `aes`  | aesthetic (美学)         | `[[aes:jkl012]]` |
| `aff`  | affect (情動)            | `[[aff:mno345]]` |
| `ses`  | session (議論セッション)  | `[[ses:pqr678]]` |
| `game` | game (KG クローラ)       | `[[game:hollow-knight]]` |

ID は型ごとの primary key。 utterance / hypothesis / gap / mechanic /
aesthetic / affect / session は UUID v4、 game は kebab-case slug。

### resolver の振る舞い

viewer 実装ごとに異なるが、 推奨は以下:

1. **同一リポ内 md ファイルを優先**:
   `[[hyp:abc123]]` → `data/discussions/<workspace>/hypothesis/abc123.md` を探す
2. **見つからなければ DB lookup**: Discatier Core repo (`createCore().repos.*`) で取得
3. **どちらも無い (dangling link)** → link をそのまま表示 + 警告 marker

GitHub markdown は wikilink 非対応のため、 `[[…]]` がそのまま文字列として表示
される (= 視覚的に「これは KG 参照だ」 とわかれば OK の段階)。
Memoria 等独自 renderer は wikilink → リンク化を実装する。

## md フォーマット

ファイルパス: `data/discussions/<workspace>/<type>/<id>.md`

`workspace` は Discatier の workspace_id (例: `knowledge` / `team-alpha`)。

### Hypothesis 例

```markdown
---
id: "abc123"
type: hypothesis
workspace_id: "knowledge"
status: validated
statement: "オンボーディングは選択肢を絞ったほうが離脱が減る"
addresses: "[[gap:def456]]"
proposes:
  - "[[mch:ghi789]]"
refs:
  - "[[utt:550e8400-e29b-41d4-a716-446655440000]]"
  - "[[utt:6ba7b810-9dad-11d1-80b4-00c04fd430c8]]"
validated_by: ["emotion"]
created_at: "2026-05-29T10:00:00Z"
updated_at: "2026-05-29T10:30:00Z"
---

# 仮説: オンボーディングは選択肢を絞ったほうが離脱が減る

## ステータス
**validated** (validated_by: emotion / 2026-05-29)

## 根拠 (utterances)
- [[utt:550e8400-e29b-41d4-a716-446655440000]] — 「3 つ選ばせると逃げる」
- [[utt:6ba7b810-9dad-11d1-80b4-00c04fd430c8]] — 「2 択にしたら通過率上がった」

## 取り組む gap
[[gap:def456]]

## 派生 mechanic
- [[mch:ghi789]]
```

### Design Gap 例

```markdown
---
id: "def456"
type: gap
workspace_id: "knowledge"
title: "チュートリアル離脱率"
status: open
gap_in: "Onboarding"
expected_affect: "engaged"
observed_affect: "overwhelmed"
hypotheses:
  - "[[hyp:abc123]]"
---

# Design Gap: チュートリアル離脱率

## 期待 vs 観測
- 期待: **engaged** (in Onboarding)
- 観測: **overwhelmed**

## 提案された hypotheses
- [[hyp:abc123]]
```

### Mechanic 例

```markdown
---
id: "ghi789"
type: mechanic
workspace_id: "knowledge"
name: "Choice Curation"
game: "[[game:onboard-2026]]"
description: "...強制 2 択 UI"
intends: "離脱を下げる"
intended_affect: "engaged"
proposed_by:
  - "[[hyp:abc123]]"
---

# Mechanic: Choice Curation
...
```

### Utterance 例

```markdown
---
id: "550e8400-e29b-41d4-a716-446655440000"
type: utterance
workspace_id: "knowledge"
session: "[[ses:pqr678]]"
speaker: "Alice"
posted_at: "2026-05-28T15:00:00Z"
referenced_by:
  - "[[hyp:abc123]]"
---

# Utterance

> 3 つ選ばせると逃げる
```

## Phase ロードマップ

### Phase 0 (本 PR) — 粗く動かす
- wikilink 規約 + parser/formatter
- 1 ノード → 1 md exporter (hypothesis / gap / mechanic / aesthetic / utterance / session)
- CLI: `npm run visualize <type> <id>` で `data/discussions/<workspace>/<type>/<id>.md`
- ノード間 link は frontmatter / 本文に埋め込むが、 **再帰的に対向ノードの md は生成しない**
  (depth=0)

### Phase 1 — graph traversal
- `--depth N` で `addresses` / `refs` / `proposes` を辿って隣接ノードも同時 export
- 既存 md があれば diff merge (= 既存内容を保ったまま frontmatter 更新)
- LUDIARS 共通 viewer (Memoria / Corpus) で wikilink → リンク解決を実装

### Phase 2 — 自動 export + Discord 連携
- hypothesis 確定 / gap 検出 イベントを listen して自動 md write + git commit
- Discord `/lineage <id>` slash command で md ファイルへのリンクを embed 返信
- 既存 KG クローラ md (`data/games/*.md`) の mechanics[].id を `[[mch:<id>]]`
  形式で書き換える migration

### Phase 3 — 双方向同期
- md を編集すると Discatier event log にも反映 (= md が KG の正本になる候補)
- Memoria / Corpus 側で graph viewer (D3 / Cytoscape) を実装

## 非ゴール (Phase 0)

- graph traversal / 再帰 export (Phase 1)
- 既存 md の差分マージ (Phase 1)
- 自動 export / Discord 連携 (Phase 2)
- viewer 実装 (Phase 2 以降)
- md → Discatier event log への逆同期 (Phase 3)
- frontend / GUI (Discord-only pivot に反するため永久に non-goal)

## CLI

```sh
npm run visualize hypothesis <id>     # data/discussions/<ws>/hypothesis/<id>.md
npm run visualize gap <id>
npm run visualize mechanic <id>
npm run visualize aesthetic <id>
npm run visualize utterance <id>
npm run visualize session <id>

# workspace 指定 (default: env DISCATIER_WORKSPACE or "knowledge")
npm run visualize hypothesis <id> --workspace team-alpha
```

## データソース・ビュー (学習データの出所可視化)

「学習データがいくつの取得元から・どこから来たか」を学習ビューアの **「データソース」タブ**
(`/learning/sources`) で俯瞰する。

- 集計は `src/visualize/data-sources.ts` の `buildDataSourceSnapshot(rawDb, ws)` (純粋関数)。
  KG の `sessions.title` 接頭辞 (`<source>:<slug>`) を取得元 source として切り出し、
  source 別 / source×ゲーム別に session 数・utterance 数・対象ゲーム数を集計する。
- **source の種別**: `import` (steam / youtube / niconico / website / fandom / reddit / opencritic =
  外部から取り込んだデータ) / `internal` (discussion-of-gap = 自走議論, discord-session) /
  `derived` (reception = wire:affects 由来)。各 source にラベルと取得元 (origin) の説明を持たせる。
- gap uuid (`discussion-of-gap:<uuid>`) は「ゲーム取込」に数えない (UUID 形を除外)。
- スナップショットは他の層と同じく `build:learning-cache` で `data/learning-cache.sqlite` の
  `data-sources` キーに焼かれ、ビューアは cache のみ参照 (KG 非ロック)。
- ビューア: source 一覧 (発話数・取得単位・対象ゲーム) + ゲーム別の取得元内訳 + 円グラフ
  (青=取込 / 橙=派生 / 灰=内部生成)。

### データソース隔離 (quarantine) — 不要な取得元を議論から外す

「この取得元のデータは議論・学習に使いたくない」と判断したら、**KG から退避ファイルへ丸ごと
移してから削除**できる (可逆)。実装 `src/crawler/quarantine.ts`、CLI は `scripts/crawl.ts`。

- `crawl.ts quarantine <source> [<slug>] [--dry-run]` — `session.title=<source>:<slug>` に一致する
  session とその配下 (utterance / reaction / translation_proposal / embedding / opinion_score /
  discord_message_map / affect) を、ATTACH した別 SQLite (`data/quarantine/<source>[-<slug>]-<ts>.sqlite`
  + manifest `.json`) に `INSERT...SELECT` で写し、トランザクションで KG から DELETE する。
  JSON 化しないので steam (数十万 utterance) でもメモリを食わない。`--dry-run` は件数プレビューのみ。
- slug を省くと **source 全体**を隔離し、dedup 台帳 `data/external/.ingested.sqlite` の該当 source 行も
  退避+削除する (再取り込み可能に)。slug 指定時は ingested 台帳は触らない (source 単位でしか引けないため)。
- `crawl.ts quarantine-restore <archive.sqlite>` — 退避ファイルから KG + ingested 台帳へ `INSERT OR IGNORE`
  で全戻し。`crawl.ts quarantine-list` で退避済み一覧。
- 隔離/復元は KG への書き込み。**Bot / live `/learning` を止めてから**実行し、後で
  `npm run build:learning-cache` でデータソース・ビュー / Gap率を焼き直す。

## 参照
- Discatier Core schema: `src/core/db/schema.ts`
- Discatier Core repos: `src/core/repositories/base.ts`
- crawler との接続: `spec/crawler/DESIGN.md`
- LUDIARS ダッシュボード: <https://ludiars.github.io/LUDIARS/>
