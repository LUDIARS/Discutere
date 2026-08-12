# 外部の声 RAG ハイブリッド検索 (キーワード recall + ベクトル rerank)

> 2026-08-11 / Di 精度向上方針 A+C の実装 spec。

## 背景 — 何の精度を上げるか

議論の材料になる「外部の声」(KG に 41 万件超) の検索は、これまで
キーワード分解 (`extractKeyTerms`) + SQL LIKE + 別名静的辞書のみだった。
言い換え・文脈類似 (例:「難易度がちょうどいい」⇔「歯ごたえがある」) を拾えず、
取りこぼしは情報ゲートの密度評価・ペーパーの厚み・ペルソナの引用すべてに波及する。
core には `embeddings` テーブルと cosine 検索が実装済みだったが未配線だった。

## 設計 — 3 段構え (`src/discatier-engine-adapter/voice-search.ts`) {#SPEC-VOICE-RAG-HYBRID-SEARCH}

1. **recall (キーワード)**: 従来通り議題語を分解 → 別名展開 → SQL LIKE。
   別名展開はゲーム名 (games タイトル + 静的辞書) に加え **Ludus game-lexicon
   由来の用語辞書** (`ludus-term-aliases.ts`、自動生成) をマージする
   (「経験値⇄XP⇄EXP」「無敵時間⇄i-frame」等の言い換え)。
2. **rerank (ベクトル)**: `config.embedding.enabled` 時、候補上位 (≤1000 件) の
   ベクトルを `embeddings` テーブルから引き、クエリベクトルとの cosine 順位を
   キーワード順位と **RRF (Reciprocal Rank Fusion)** で融合する。
   スコアのスケール差 (LIKE ヒット数 vs cosine) は順位に落として吸収する。
   全件 ANN ではなく **候補集合の再ランク** (recall=キーワード / precision=ベクトル)
   なので、KG が大きくても 1 クエリの追加コストは有界。
3. **diversify**: 近似重複を bigram Jaccard で畳み (`dedupeNearDuplicates`、
   コピペ転載対策)、**MMR** (λ=0.7) で「関連度 × 多様性」の上位 limit 件を選ぶ。
   ほぼ同文の声が prompt 枠を占有するのを防ぐ = 情報ゲートの観点多様性評価にも効く。

### 同期境界の解決 — クエリ埋め込みキャッシュ {#SPEC-VOICE-RAG-HYBRID-QUERY-CACHE}

`listRelevantExternalVoices` (context-provider interface) は同期で、ペルソナの
毎ターン prompt 構築から呼ばれる。埋め込み取得 (HTTP) を同期経路に入れられないため、
**クエリ (議題) のベクトルだけ** を sidecar テーブル `embedding_query_cache` +
プロセス内メモリにキャッシュする (`src/core/vectors/query-embed-cache.ts`):

- キャッシュ命中 → その場でハイブリッド検索 (全処理同期)。
- 未命中 → キーワード検索で返しつつ、裏でクエリを埋め込んで温める
  (in-flight 重複抑止つき)。議論は同一議題で繰り返し検索するため、
  実用上は初回 1 回だけキーワード、以降ハイブリッドになる。

sidecar にはクエリ本文を保存せず、モデル名と正規化本文の SHA-256、ベクトル、作成時刻だけを置く。

声 (文書) 側のベクトルは offline バッチで構築する (下記)。

## 設定 (`config.embedding`) {#SPEC-VOICE-RAG-HYBRID-CONFIG}

| キー | env | 既定 | 説明 |
|---|---|---|---|
| `enabled` | `DISCUTERE_EMBEDDING_ENABLED` | `false` | ハイブリッド検索の opt-in |
| `baseUrl` | `DISCUTERE_EMBEDDING_BASE_URL` | `http://localhost:11434/v1` | OpenAI 互換 `/embeddings` |
| `model` | `DISCUTERE_EMBEDDING_MODEL` | `bge-m3` | 多言語・ローカル実績 (Genius recall@8=0.95) |
| `apiKey` | `DISCUTERE_EMBEDDING_API_KEY` | — | vLLM 等のみ |
| `timeoutMs` | `DISCUTERE_EMBEDDING_TIMEOUT_MS` | `30000` | |
| `batchSize` | `DISCUTERE_EMBEDDING_BATCH_SIZE` | `32` | バッチ構築用 |

`llm.local` (chat) とは独立 (埋め込み専用モデルのため)。Ollama GPU が PTX 不一致で
使えない環境でも bge-m3 の埋め込みは CPU で実用速度 (`num_gpu 0`)。

## 運用手順 {#SPEC-VOICE-RAG-HYBRID-OPS}

```sh
# 1. 埋め込みモデルを用意 (Ollama)
ollama pull bge-m3

# 2. 声側インデックスを構築 (増分。初回はデータ量次第で長時間)
npm run build:voice-embeddings -- --limit 5000    # まず様子見
npm run build:voice-embeddings -- --query "モンスターストライク"  # 議題を優先して部分構築
npm run build:voice-embeddings                     # 全量

# 3. 有効化
#    discutere.config.json: { "embedding": { "enabled": true } }

# 4. 精度を数字で確認 (改善前後の比較)
cp data/eval/retrieval-golden.example.json data/eval/retrieval-golden.json
#    実 KG に合わせてケースを育てる
npm run eval:retrieval
```

モデルを変えたら `npm run build:voice-embeddings -- --rebuild` (空間が混ざるため)。
`--rebuild` と `--query` は、全削除後の部分再構築を防ぐため同時指定しない。
Ludus 辞書更新後は `npm run build:ludus-term-aliases <path-to-game-lexicon>` で
`ludus-term-aliases.ts` を再生成してコミットする (実行時に Ludus リポへ依存しない)。

## 評価 (`npm run eval:retrieval`) {#SPEC-VOICE-RAG-HYBRID-EVAL}

ゴールデンセット (`data/eval/retrieval-golden.json`) の各議題について
keyword / hybrid 両モードで検索し、marker ヒット数・marker カバレッジ・
(ラベル済みなら) recall@k を並べて出す。「精度が上がった」を主観でなく
数字で判定するための基盤。ケースは実 KG に合わせて育てる。

## degrade 方針 (議論を止めない) {#SPEC-VOICE-RAG-HYBRID-DEGRADE}

- `embedding.enabled=false` (既定): 従来のキーワード検索のみ。挙動不変。
- enabled だがエンドポイント不通: warn ログを出してキーワード検索で続行
  (温め失敗は毎回ログに出る = 観測可能。silent fallback にしない)。
- 声側インデックス未構築: rerank 対象ベクトルが無いのでキーワード順のまま。
- 声側インデックスが増分構築の途中: 候補集合のベクトルが揃うまでは、索引済みという理由だけで
  下位候補を優遇しないようキーワード順のまま。

## follow-up (非採用/次段)

- 全件 ANN (sqlite-vec / kuzu vector index) — 候補集合 rerank で足りなくなったら。
- HyDE (仮想文書埋め込み)・pseudo-relevance feedback — クエリ側の拡張。
- 情報ゲートの決定的カバレッジ評価 (facet 別ヒット数) — density-blackbox の拡張。
