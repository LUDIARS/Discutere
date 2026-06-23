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

## 仕様書の解析学習 (② 仕様書の解析学習, LLM, 2026-06-23)

貼り付けられた**ゲーム仕様書テキスト**を LLM に解析させ、語られている**遊びのメカニクス**を
`GameMechanicEntry[]` として抽出し、学習フローの **mechanics 記録経路** (`data/games/<slug>.md`
+ Discatier Core) にそのまま流す。仕様書は「ユーザの意見」ではなく**作者の設計文書**なので、
外部の声 (opinions) ではなくメカニクスに倒す (① が外部の声、② が設計メカニクス、という役割分担)。

- 本体 `src/flow/spec-analyze.ts` の `analyzeSpecMechanics({ theme, specText, llm })` は
  DB 非依存・LLM 注入で単体テスト可能。JSON 抽出は `mechanic-extract.ts` の `extractJsonArray`
  を共有 (コードフェンス/前置き混入に耐える)。valence はホワイトリスト正規化、name で重複除去、
  `maxMechanics` (既定 40) でクランプ。長文は先頭 16000 文字に切り詰め。
- **`runLearningFlow` は無改変**。抽出結果は glue 層が `dispatchFlow` の `mechanics` として渡す
  (既存のメカニクス記録経路を再利用)。空 specText / LLM 失敗は `[]` で degrade (学習を止めない)。
- 入口:
  - **Web** = `/flow` の「仕様書の解析学習」テキストエリア → `/api/flow/start` の `specText` →
    `routes.ts` の learning ケースで `analyzeSpecMechanics` → `mechanics` で dispatch。
  - **Discord** = 学習フォーラムの **starter 本文**を仕様書として解析 (スレッドタイトル=議題と
    別物のときだけ)。`gateway.ts` が starter 本文を `specText` として `startForumFlow` まで運び、
    `discord-live.ts` の learning ケースで解析 → 完了通知に「仕様書メカニクス N 件」を表示。

> **未配線 (後続)**: ③「Anatomia 経由の仕様書解析」(Anatomia の解析データ直流し) は別経路として
> 追加予定。① が外部の声クロール、② が仕様書 LLM 解析。

## 出力

- ユーザ意見・メカニクスを KG / コーパスに永続 (個人データ非保管、`CLAUDE.md` 準拠)。
- ① 自動収集モードでは外部の声を横断クロールして取り込む (件数は `crawledImported`)。
- ② 仕様書解析では仕様書から抽出したメカニクスを記録する (件数は `mechanicsRecorded`)。
- これが議論フロー step 2 (調査) / step 3 (ペーパー生成) の入力になる。

## 関連

- [OVERVIEW.md](./OVERVIEW.md) / [discussion.md](./discussion.md) /
  [../feature/game-feedback.md](../feature/game-feedback.md)
