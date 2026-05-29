# Crawler — Review/Comment → Sentiment & Discussion (Phase 0)

`spec/crawler/DESIGN.md` の Game KG に対し、**ユーザーの反応(レビュー/コメント)** を
Discatier Core の **affect(感情) / discussion 埋め込み(議論ベクトル)** に変換して付与するモジュール。

## 入力

外部の収集ツール(`LUDIARS/game-knowledge-graph/collectors/`)が吐く `collected.json`:
公式 API 優先(Steam appreviews / YouTube Data API / Google Custom Search)+ API 無しサイトのみ礼節スクレイピング。
正規化レコード `{ source, game, text, lang, posted_at, meta{voted_up,…} }`。

## 変換 (src/crawler/sentiment/analyze.mjs — 依存ゼロ)

ハイブリッド: **辞書ベース一次極性**(`sentiment/lexicon.json`、日英)→ 集約。
(LLM/Claude Code による要約・クラスタ精緻化は後段で差し込み可能。)

### 用意されたベクトル(再定義版・複合固定 20 次元 / 各次元 0..1)

全ゲーム共通の固定ベクトル空間。収集集合をこの空間へ**正規化**する。

```
emo.valence, emo.arousal,
emo.{joy,trust,fear,surprise,sadness,disgust,anger,anticipation}   (Plutchik 8)
asp.{fun,difficulty,content,price_value,performance,story,graphics,replayability}
meta.positive_ratio, meta.volume_log
```

- **ゲーム全体 / discussion cluster(dominant aspect 別) / 月次バケット** を**すべて同一 20 次元**で表現
  → cosine 比較・時系列(感情曲線)・横断比較が同一空間で可能。**議論はこのベクトルデータで扱う**。
- number[] なので Discatier `embeddings`(`vector_json`)に格納可能
  (`node_type="game_sentiment"` / `"discussion_cluster"`)。emo ブロックは `affects`(mood/valence/score)に対応。
- **affects** — overall + 月次の `{mood, valence, score}`。**感情曲線** — `sentiment_curve[]`(period × vector)。
- ベクトルは外部 embedding API を使わず本コード内 (Claude Code) で算出する。

> 注: 旧版(Discatier `affects` + 自由次元 embeddings)ではなく、再定義版のこの複合固定ベクトルを採用。
> 旧版の利点(embeddings 格納・affects 対応)は number[] 化により両立する。

## 出力

- `data/games/<slug>.md` — importer 互換 frontmatter(genre + `sources[]` は `fetched_at`/`excerpt_policy`)+ 人間用サマリ。
- `data/games/<slug>.sentiment.json` — affects / sentiment_curve / aspects / clusters(L2 ベクトル)/ overall。

## 制約 (DESIGN.md 準拠)

- **summary-only**: レビュー/コメント原文は md/JSON に転載しない(数値・ラベル・ベクトルのみ)。
- **個人データ非保管**: author / アカウント名は出力しない。
- **取得日**: `sources[].fetched_at`(= ソース同一性の一部。サイト×取得日が違えば別ソース)。

## CLI

```sh
node src/crawler/sentiment/analyze.mjs \
  --in <collected.json> --slug vampire-survivors --title "Vampire Survivors" --genre "ローグライク / bullet heaven"
```

## TODO (follow-up PR)

- DB import 配線: affects → `core.repos.affect`、clusters → `core.registerEmbedding({nodeType:"discussion_cluster", vector})`。
  (本 PR は md + sidecar JSON 生成まで。Kuzu 反映は別 PR で、進行中の議論-md 設計確定後に。)
- 月次以外の粒度(週次)・言語別感情曲線。
- LLM パスでクラスタ要約 / mechanics 抽出を Claude Code で付与。
