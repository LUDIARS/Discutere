# クローラー → 解析 → 議論パイプライン 総まとめ

ゲームの「設計データ(KG)」と「ユーザーの声(レビュー/コメント)」を収集し、
**複合固定ベクトル**へ正規化して Discatier の **3軸議論ループ**に供給するまでの全体仕様。

対象は 2 リポにまたがる:

- `LUDIARS/game-knowledge-graph`(外部・収集/解析): クローラー群・collectors・sentiment analyzer
- `LUDIARS/Discutere`(Discatier Core): 議論ロジック・発話解釈・KG 永続化

---

## 0. 全体フロー

```
                       ┌──────────────── 収集 (game-knowledge-graph) ────────────────┐
 [A] Notion KG  ─┐     │  collectors/ (Steam公式API / YouTube Data API /             │
 [B] Fandom Wiki ┼────▶│   Google CSE / GameWith礼節scrape)  → collected.json        │
 [C] レビュー/   │     │                                                              │
     コメント     ┘     │  analyze (sentiment) → 複合20次元ベクトル + affects + clusters │
                       └───────────────┬─────────────────────────────────────────────┘
                                       │ data/games/<slug>.md (importer互換) + <slug>.sentiment.json
                                       ▼
                       ┌──────────────── Discatier Core (Discutere) ─────────────────┐
                       │  importer → Game / Mechanic / Aesthetic                       │
                       │  (follow-up) affects → affects 表 / 複合ベクトル → embeddings  │
                       │                                                              │
                       │  3軸議論ループ:  Axis1 学習 ─intended_affect─┐               │
                       │                  Axis2 感情 ─expressed_affect─┼─▶ DesignGap   │
                       │                  Axis3 統合 ─Hypothesis───────┘   → 検証 → Axis1 │
                       └──────────────────────────────────────────────────────────────┘
```

収集された **レビュー/コメント = Axis2(感情会話)の発話相当**。解析が付けるベクトル/affect が
Translation Bridge・Gap Detection の入力になる。

---

## 1. 各クローラーのデータインポート方針

| # | クローラー | ソース | 取得手段 | 認証 | 礼節 | 出力 | import_id(取得日付き) |
|---|---|---|---|---|---|---|---|
| A | Notion KG | Notion 公開ページ | `loadPageChunk` API (cursor) | 不要 | キャッシュ | `graph/<genre>.json` | `src_0001` |
| B | Fandom Title | *.fandom.com | MediaWiki `action=parse` API | 不要 | 2.5s間隔/UA/キャッシュ | `graph/titles/<slug>.json` | VS=`src_0002` / 原神=`src_0003` |
| C-1 | collectors:steam | Steam | 公式 `appreviews`(appid自動解決) | 不要 | 1.5s間隔/キャッシュ | `collected.json` | (site×取得日) |
| C-2 | collectors:youtube | YouTube | 公式 Data API v3 | `YOUTUBE_API_KEY` | quota配慮/キャッシュ | 〃 | 〃 |
| C-3 | collectors:google | Google | 公式 Custom Search JSON API | `GOOGLE_CSE_KEY`+`CX` | 〃 | 〃 (議論ページ発見用) |
| C-4 | collectors:gamewith | GameWith | 礼節スクレイピング | 不要 | **robots尊重**/3s間隔 | 〃 (experimental) | 〃 |
| C-x | X(Twitter) | — | — | 有料API | — | **非対応** | — |

共通方針:
- **公式 API 優先**、API の無いサイト(GameWith)だけ礼節スクレイピング。X は有料/Bot対策で非対応。
- **認証キーはリポジトリに保存しない**(`--env-file=collectors/.env`、値は AI/ログに残さない)。
- **取得済みは再取得しない**(URL/レスポンス単位キャッシュ)。
- **ソース管理は RDBMS(`sources.db`)**。識別キーは **`UNIQUE(site, fetched_date)`** = 「(サイト × 取得日)が同一なら同一 import_id、取得日が違えば別ソース」。
- **summary-only / 個人データ非保管**: レビュー/コメント原文は転載せず、author/アカウント名は出力しない(`spec/crawler/DESIGN.md` 準拠)。

---

## 2. データ構造

### 2.1 収集の正規化レコード (collected.json)
```jsonc
{ "id":"steam:123", "source":"steam", "game":"…", "kind":"review|comment|search_result",
  "author":"…(解析後は破棄)", "text":"本文", "lang":"english", "posted_at":"…Z",
  "url":"…", "meta": { "voted_up":true, "likeCount":… } }
```

### 2.2 用意されたベクトル(再定義版・複合固定 20 次元 / 各 0..1)
```
emo.valence, emo.arousal,
emo.{joy,trust,fear,surprise,sadness,disgust,anger,anticipation}        # Plutchik 8
asp.{fun,difficulty,content,price_value,performance,story,graphics,replayability}
meta.positive_ratio, meta.volume_log
```
- **ゲーム全体・議論クラスタ・月次バケットを全て同一 20 次元**で表現 → cosine 比較・時系列・横断比較が一空間で完結。
- number[] なので Discatier `embeddings.vector_json` に格納可。emo ブロックは `affects(mood/valence/score)` に対応。

### 2.3 解析出力 (sentiment.json)
`vector_spec` / `game_vector` / `overall{valence,score,positive_ratio,volume}` /
`aspects{<a>:{mentions,score}}` / `affects[]{subject,mood,valence,score}` /
`sentiment_curve[]{period,vector,mood,sentiment}` / `clusters[]{topic_aspect,size,sentiment,vector}`。

### 2.4 KG md frontmatter (importer 往復) — `GameKG`
`id / title / genre / workspace_id / sources[]{url,title,fetched_at,attribution,excerpt_policy} / mechanics[] / aesthetics[]`

### 2.5 Discatier Core ノード(永続層 / Event Sourcing + projection)
`Game · Mechanic(intended_affect) · Aesthetic · Affect(mood,score,valence) · PlayContext ·
Session(mode) · Utterance(raw_content 不可改変) · Reaction(type,intensity) ·
DesignGap(expected_affect↔observed_affect) · Hypothesis(status) · embeddings(node_type,vector_json)`

---

## 3. 議論ロジック(Discatier の 3軸弁証ループ)

単一のデータ基盤(Kuzu グラフ + イベントログ)上で、3つの対話軸が弁証法的ループを形成する。
**グラフ=現在のビュー、イベントログ=真実のソース**(状態は projection で再構築)。

| 軸 | モード | 参加者 | 役割 | 生成物 |
|---|---|---|---|---|
| Axis 1 | `learning` | 理論家 | 語彙/メカニクスの定義を精緻化 | `Mechanic.intended_affect` |
| Axis 2 | `emotion` | プレイヤー | 理論用語を介さず生の感情を報告 | `expressed_affect`(観測) |
| Axis 3 | `synthesis` | 開発者 | Axis1↔2 のズレに跳躍的仮説を提示・検証 | `DesignGap` → `Hypothesis` |

ループ:
1. **Translation Bridge (Axis2→Axis1)**: 感情発話を埋め込みベクトル化 → 既存 `Affect` とのコサイン類似 + LLM 判定 → `Mechanic` 候補推論 → `proposed_expresses`/`proposed_refers_to` を**仮エッジ**で付加 → Axis1 の未確定キューへ。
2. **Gap Detection (Axis1∩Axis2)**: Mechanic の期待 Affect(intended) と観測 Affect(expressed) を比較。「期待が観測に無い / 強い負の Affect / サンプル3以上」で `DesignGap` を生成。
3. **Hypothesis Lifecycle (Axis3→検証→Axis1)**: Gap に対し仮説を提示 → 議論 → 検証 → 確定で Axis1 の語彙/定義へ還流。
4. **未語彙化感情の保護**: 既存 `Affect` にマップされない発話パターンを定期検出し、Axis1 の議論対象として明示提示。

### 本パイプラインの接続点
- 収集レビュー/コメント = **Axis2 の発話供給源**。
- 解析の **複合ベクトル** = Translation Bridge の「埋め込み」に相当(外部 embedding API を使わず本コード内で算出)。
- 解析の **aspects** = 関連 `Mechanic` 推論のヒント、**affects** = `expressed_affect`。
- **clusters** = 未確定マッピング/未語彙化クラスタの種。
- KG(ジャンル/タイトル)由来の `intended_affect` × 解析の `observed_affect` で **Gap Detection** が走る。

---

## 4. ユーザー発言のケース分けと解釈

### 4.1 入口での分岐(Discatier `session.mode`)
発話は `UtteranceCreated` イベント化 → `responds_to` 推論 → モード別処理:
- **learning**: スラッシュコマンド(`/intends` 等)を解釈、無ければ Mechanic/Affect 候補抽出。
- **emotion**: Translation Bridge へ入力(下記 4.3)。
- **synthesis**: スラッシュコマンド + Hypothesis 進行管理。
- 絵文字 → `Reaction`。`resonates_with`/`clashes_with` は引用+短反応から**候補提示のみ**(完全自動化しない)。
- `raw_content` は**不可改変**。訂正は新規発話 + `responds_to` で表現。

### 4.2 収集発話(レビュー/コメント)のケース分類 → ベクトル寄与
本パイプラインは Axis2 発話を辞書一次解析で次のケースに分解し、複合ベクトルへ写像する:

| ケース | 検出シグナル | 解釈 / ベクトルへの寄与 |
|---|---|---|
| 称賛 | 肯定極性語 / `voted_up=true` | `emo.valence↑`, `positive_ratio↑`, joy/trust |
| 不満・批判 | 否定極性語 / `voted_up=false` | `emo.valence↓`, anger/disgust/sadness |
| バグ・性能報告 | bug/crash/lag/重い/落ちる | `asp.performance`(負寄り), 強い負 Affect → Gap 候補 |
| 難易度言及 | hard/easy/難しい/理不尽 | `asp.difficulty`(極性で正負) |
| 価格・課金 | price/worth/高い/コスパ/課金 | `asp.price_value` |
| ボリューム/やり込み | content/hours/周回/やり込み | `asp.content` / `asp.replayability` |
| ストーリー/世界観 | story/シナリオ/キャラ | `asp.story` |
| グラフィック | graphics/綺麗/作画 | `asp.graphics` |
| 感情の吐露 | 神/最高/泣ける/怖い 等 | Plutchik 8 の該当次元 + `emo.arousal` |
| 比較/言及のみ | (極性語なし・他作言及) | 中立(valence 0)・aspect 言及のみ計上 |
| ネタ/無内容 | 極性語ヒット 0・短文 | volume には計上、valence は中立 |

- 言語(`lang`)は保持(将来 言語別感情曲線)。`posted_at` で月次バケット → **感情曲線**。
- **dominant aspect** で `cluster:<aspect>` に集約 → クラスタ単位の同一20次元ベクトル(議論データ)。

### 4.3 emotion 発話の解釈フロー(Translation Bridge のレビュー4分岐)
埋め込み → 既存 `Affect` 類似 + LLM 判定 → 仮エッジ付加後、人/AI レビューで:
- **承認**: 仮エッジ → 正規エッジへ昇格。
- **修正**: 別 `Affect`/`Mechanic` に張り直し。
- **棄却**: 仮エッジ削除 + `uncategorized` 付与。
- **新語彙が必要**: `Affect` 候補を提案 → 未語彙化クラスタへ集約(Axis1 の語彙成長)。

> 誤解釈リスク対策: 原文不可改変 + 未語彙化感情の定期検出により、「既存語彙への無理な押し込み」を防ぐ。

---

## 5. 実装ステータス
- 収集(collectors)+ 解析(複合ベクトル/affect/cluster)+ md/sidecar 生成: **実装済(Phase 0)**。
- Discatier への DB 反映(affects→repo / 複合ベクトル→`registerEmbedding` / Gap Detection 連携): **follow-up PR**(進行中の議論-md 設計確定後)。
- 3軸議論ループ本体(Translation Bridge / Gap / Hypothesis): Discatier Core 既存仕様(`docs/discatier_implementation_plan.md`)。
