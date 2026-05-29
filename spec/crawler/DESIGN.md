# Crawler — Game KG Auto-Builder (Phase 0)

著名なゲームの攻略情報を **構造化抽出 → Discatier Core の Game / Mechanic / Aesthetic
ノードに登録** するためのモジュール。 Web からの Claude 起動でデータを収集し、
中間形態 `data/games/<slug>.md` (frontmatter + 自由記述) として永続化し、
後段で importer が Discatier Core に append する。

LUDIARS 短縮コード: **Di** / モジュールパス: `src/crawler/`

## なぜ Discutere に置くのか

Discatier Core (`src/core/`) には既に Game / Mechanic / Aesthetic / PlayContext /
DesignGap の vocabulary が定義済 (Phase 1-6)。 これは元々「ゲームデザイン分析」
を Event Sourcing で扱うために設計されたもの。 攻略データを KG として整理する
本機能は、 この vocabulary に **そのまま乗る**。 新 schema は不要 (Phase 0)。

ジャンルは Game.genre (TEXT) に格納。 攻略の主要要素 (戦闘 / 探索 / 育成 etc.)
は Mechanic ノードに、 デザイナ意図 / 雰囲気は Aesthetic ノードに分解する。

## アーキテクチャ

```
                       ┌───────────────────────────────────────────────────────┐
   Web (攻略サイト)    │  Phase 1+  Claude API (WebSearch tool use)            │
        │              │     → 抽出 (要約のみ・原文転載しない)                 │
        ▼              │     → KG オブジェクトに分解                            │
   ┌─────────────┐     │     → md 出力                                          │
   │  runner.ts  │ ────┘                                                        │
   │ (Phase 1+)  │                                                              │
   └─────────────┘                                                              │
        │                                                                      │
        ▼                                                                      │
  data/games/<slug>.md  ←─ Phase 0 では手書きを許す (実装早期確認用)            │
        │                                                                      │
        ▼                                                                      │
   ┌─────────────┐                                                             │
   │ importer.ts │  →  Discatier Core repositories                             │
   │             │      ├─ createGameRepo:    Game upsert (title, genre)       │
   │             │      ├─ createMechanicRepo: Mechanic per item               │
   │             │      └─ createAestheticRepo: Aesthetic per item             │
   └─────────────┘                                                             │
        │                                                                      │
        ▼                                                                      │
   Kuzu (SQLite WAL) — games / mechanics / aesthetics                          │
                                                                               │
        ◀── Phase 2: Discord slash command (/kg lookup …) で参照               │
```

## KG md フォーマット

`data/games/<slug>.md`:

```markdown
---
id: hollow-knight                  # slug (Game.id の dedup key、 ASCII kebab)
title: "Hollow Knight"
genre: "メトロイドヴァニア"          # string (将来 Genre ノード化、 今は Game.genre)
workspace_id: "knowledge"          # 全 KG 共通の workspace
sources:
  - url: "https://example-wiki/HollowKnight"
    title: "Hollow Knight 攻略 wiki"
    fetched_at: "2026-05-29"
    attribution: "CC-BY-SA 3.0"    # 引用元のライセンス
    excerpt_policy: "summary-only" # summary-only / fair-use-quote / forbidden
mechanics:
  - name: "ダッシュ"
    description: "短距離の素早い移動。 戦闘・回避の両用。"
    intends: "緊張と解放のリズム"
  - name: "チャーム装備"
    description: "ノッチを消費して特殊能力を付与"
aesthetics:
  - name: "孤独な探索"
    description: "BGM・色調・敵配置で「無人の地」を強調"
---

# 概要 (人間用)

簡潔な overview。 Web からの引用・スクリーンショットは置かない。
あくまで「KG に登録された内容の自然言語サマリ」。

# 攻略上のポイント (人間用)

- ボスの行動パターン要約 (自分の言葉で)
- 進行順序の選択肢
```

**round-trip**: importer は frontmatter だけ読む。 本文 (markdown) は KG には
入れず、 PR レビューや人間閲覧のための補足。 importer 後に手で本文を加筆しても
DB には影響しない (= 「md は KG + 人間用注釈」 の二層)。

## 制約 (= 必ず守る範囲)

### ToS / robots.txt
- Phase 1+ の crawler runner は **対象サイトの robots.txt を最優先**
- rate limit: 同一ドメイン 1 req / 5 sec を下回らない
- User-Agent は `LUDIARS-Discutere-Crawler/<version> (+contact)` を明示

### 著作権 / 引用
- 攻略サイトの **本文を md に転載しない**。 frontmatter の `sources[].excerpt_policy`
  を `summary-only` に固定する (Phase 0)
- mechanics / aesthetics の `description` は **自分の言葉で要約** する。
  原文の段落そのものを貼り付けない
- canonical source (sources[].url) を常に明記し、 attribution があれば従う
- 商用攻略サイト (Game8 / GameWith 等) を含める場合は **社内利用のみ** を前提とし、
  公開 (LUDIARS Pages / 外部 API) はしない。 Phase 1 実装時に再度ポリシー確認

### 個人データ
- 攻略 wiki の編集者名 / アカウント名は md に書かない
- 全 KG は workspace_id = `"knowledge"` の **匿名スペース** に閉じる

## Phase ロードマップ

### Phase 0 (本 PR) — 粗く動かす
- KG md schema 確定
- importer.ts (md → Discatier repos round-trip)
- runner.ts は **interface 定義のみ** (実装は Phase 1)
- scripts/crawl.ts CLI: `import` / `list` の 2 サブコマンド
- sample md (Hollow Knight) を手書きで 1 件 + round-trip テスト

### Phase 1 — runner 実装
- Claude API (WebSearch tool) で「ゲーム名 → md」 を生成
- robots.txt / rate limit / attribution 自動収集
- GitHub Actions で日次自動 crawl → PR (label: `auto-crawl`)
- PR review で merge 後、 importer が main の md を自動 import

### Phase 2 — Discatier / Discord 統合
- Discord slash command `/kg lookup <ゲーム名>` で Game ノードを取得
- cross-game query: 同一 Mechanic を持つ Game をリスト
- vector embedding (`src/core/vectors/`) を mechanics.description に張って類似検索
- Genre ノード化 + (Game)-[HAS_GENRE]->(Genre) edge を Mechanic edge と並べる

### Phase 3 — 結合確認
- 攻略データ × Discatier の DesignGap / Hypothesis 連携
- 「このメカニクスは何の affect を狙っているか」 を hypothesis 化
- 既存 phase 4-6 の gap-detection / hypothesis-lifecycle と統合

## CLI

```sh
# md を Discatier Core に import
npx tsx scripts/crawl.ts import data/games/sample-hollow-knight.md

# DB 内の Game 一覧
npx tsx scripts/crawl.ts list

# (Phase 1+) ゲーム名から md を生成
npx tsx scripts/crawl.ts run "Hollow Knight" --out data/games/hollow-knight.md
```

## 非ゴール (= Phase 0 ではやらない)

- vector embedding (Phase 2)
- Genre ノード化 (Phase 2)
- 自動 PR (Phase 1)
- Discord 連携 (Phase 2)
- フロントエンド可視化 (Phase 2 以降)
- migration / DB schema 変更 — Phase 0 は既存 schema をそのまま使う

## 参照
- Discatier Core: `src/core/` / `src/core/repositories/base.ts`
- 既存 schema: `src/core/db/schema.ts`
- LUDIARS ゲーム辞書: `LUDIARS/ars-game-lexicon` (将来の連携先候補)
