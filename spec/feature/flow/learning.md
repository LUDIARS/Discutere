# 学習フロー (Learning)

> 状態: ドラフト (2026-06-13)。共通仕様は [OVERVIEW.md](./OVERVIEW.md)。

ゲームタイトルおよびテーマに即した**ユーザの意見、またはメカニクス**を発話・記録する
フロー。**LLM による議論は行わない**。議論フローの**前段**として実行し、その内容を
ユーザデータとして仕込む (= 後続の議論の学習データになる)。

## 性質

- **議論しない**: ペルソナ同士の応酬・ラウンド・投票は無い。発話を**収集・記録**するだけ。
- **対象**: ゲームタイトル + テーマに即した
  - ユーザの意見 (感想)
  - メカニクス (機能・仕組み)
- **位置づけ**: 議論前にやることで、議論時のディスカッションペーパー
  ([OVERVIEW §5](./OVERVIEW.md#5-ディスカッションペーパー)) の素材 (ユーザ意見まとめ /
  メカニクス) を充実させる。

## 既存資産との対応

- ゲーム感想チャンネル (現 `discord.gameFeedback`、カテゴリ「ゲーム感想」) の匿名意見収集と
  目的が一致 → 流用候補。
- メカニクスは Discatier Core の `mechanics` / `data/games/<slug>.md` に記録。
- 感情極性付与は感情カスケード (`src/crawler/sentiment/cascade.ts`) を流用。

## 自動収集モード (① 類似ゲームの自動収集, 2026-06-23)

学習フローは元々「収集済みの匿名意見 (`opinions[]`) を渡す」設計だったため、Web/Discord から
**起動するだけでは収集 0 件**になっていた (収集元が配線されていなかった)。これを解消するため
`runLearningFlow` に **自動収集モード** (`deps.crawl`) を追加した。

- `deps.crawl.sources` を指定すると、`opinions` 未供給でもテーマで **複数ソース横断クロール →
  KG 取込** する (`src/flow/learning-autocrawl.ts` の `collectAndImport` をソースごとに呼ぶ)。
- **充足ゲートは挟まない**。学習は明示起動なので常に収集する (件数閾値で skip しない)。
  ゲート付きの「議論前の事前学習」は議論/改善フロー側の information-gate が担う (役割分担)。
- ソース解決は `resolveAutoCrawlSources` を information-gate と共有: niconico は常に可、
  youtube は `DISCUTERE_YOUTUBE_API_KEY` があれば自動追加、website/steam は URL/appId が
  テーマだけからは決まらないため自動経路では除外 (UI の明示指定時のみ単一ソースで使う)。
- 1 ソースの失敗は学習全体を止めない (graceful degrade)。結果に `crawledImported` /
  `crawledBySource` を返す。
- 配線: Web=`src/flow/web/routes.ts` の `buildLearningCrawl` (UI の learningSource を尊重)、
  Discord=`src/flow/discord-live.ts` (config 既定の横断ソース)。両者とも `dispatchFlow` の
  `learningCrawl` で渡す。

> **未配線 (後続)**: ②「仕様書の解析学習 (LLM)」と ③「Anatomia 経由の仕様書解析」は別ソース
> モードとして追加予定。現状の `crawl` は ① (外部の声クロール) のみ。

## 出力

- ユーザ意見・メカニクスを KG / コーパスに永続 (個人データ非保管、`CLAUDE.md` 準拠)。
- 自動収集モードでは外部の声を横断クロールして取り込む (件数は `crawledImported`)。
- これが議論フロー step 2 (調査) / step 3 (ペーパー生成) の入力になる。

## 関連

- [OVERVIEW.md](./OVERVIEW.md) / [discussion.md](./discussion.md) /
  [../feature/game-feedback.md](../feature/game-feedback.md)
