# Di FT パイプライン仕様 — Gemma-3-12b QLoRA teacher-student 蒸留

> task #141 / 更新: 2026-06-12

## 概要

Claude (Opus/Sonnet) を **teacher** として headless 議論を大量生成し、
Gemma-3-12b を **student** として QLoRA fine-tune することで、
議論 persona の口調・推論パターンを蒸留する。
蒸留済みモデルは Ollama に `di-debate:v1` として登録し、
`llm.backend=local` / `llm.local.model=di-debate:v1` で Di の
persona/facilitator を置き換えて完全ローカル推論を実現する。

---

## 1. データ生成フェーズ

### 1-1. headless 議論 (`npm run headless`)

- `scripts/headless-discuss.ts` がスクリプトとして CLI を内部 spawn し、
  Di の議論エンジン (分類器 → designGap → persona-engine × N → facilitator) を
  完全自動で 1 セッション回す。
- `--llm cli` (デフォルト): Claude Code CLI を worker として使用。
- `--llm anthropic`: Anthropic SDK を直接コール (API キー要)。
- `--rounds N` (デフォルト 40): 1 セッションのラウンド数。
- `--persist`: ターンデータを `data/worker-turns/` に保存する。このフラグが FT の前提。

### 1-2. turn ファイル形式

```
data/worker-turns/
  <reqId>.json       — TurnJson  { reqId, workerId, system, prompt }
  <reqId>.reply.json — ReplyJson { reqId, workerId, text }
```

- `system`: persona の役割定義・議論ルール・ワールド文脈。
- `prompt`: 直前の会話履歴 + 現ターンの入力 (facilitator / 他 persona の発言)。
- `text`: worker (Claude) の生成テキスト。`{action:"skip"}` はスキップ。

### 1-3. バッチ生成 (`npm run ft:generate`)

```
npx tsx scripts/generate-training-data.ts \
  --sessions 100 \   # 自動実行セッション数 (デフォルト 100)
  --llm cli \         # teacher backend
  --rounds 40         # 1 セッションのラウンド数
```

- 完了後 `npm run ft:export` を自動実行して JSONL を書き出す。
- `--dry-run` で 1 セッションのみ試行。

---

## 2. エクスポートフェーズ

### 2-1. export-ft-data.ts (`npm run ft:export`)

- `data/worker-turns/*.json` をスキャンし、対応する `*.reply.json` とペアにする。
- reply が見つからない turn、空テキスト、`{action:"skip"}` はスキップ。
- OpenAI chat JSONL 形式に変換:

```jsonl
{"messages":[{"role":"system","content":"..."},{"role":"user","content":"..."},{"role":"assistant","content":"..."}],"metadata":{"reqId":"...","workerId":"..."}}
```

- 出力: `data/ft-export/debate-YYYY-MM-DD.jsonl`

### 2-2. 期待サンプル数

| 設定 | ターン数 (推定) |
|---|---|
| 100 sessions × 40 rounds × 3 persona | ~12,000 |
| 500 sessions × 40 rounds × 3 persona | ~60,000 |

Gemma-3-12b QLoRA の有効 FT には 5,000〜20,000 サンプル程度で十分。

---

## 3. 学習フェーズ (QLoRA)

### 3-1. 環境

| 項目 | 値 |
|---|---|
| 実行環境 | WSL2 (Ubuntu 22.04) |
| スクリプト | `scripts/ft/unsloth_train.py` |
| ベースモデル | `unsloth/gemma-3-12b-it-bnb-4bit` (HuggingFace) |
| 量子化 | 4-bit NF4 (bitsandbytes) |
| 学習手法 | QLoRA (rank=16, alpha=32, dropout=0.05) |
| フレームワーク | Unsloth + TRL SFTTrainer |

### 3-2. 学習ターゲット

学習させたい能力:

1. **役割別口調** — 論客ペルソナ (論理派 / 感情派 / 懐疑派) ごとの話し方と語彙。
2. **議論推論パターン** — 前発言を受けて反論 / 補強 / 問い返しを構造的に行う。
3. **JSON 出力構造** — `{action:"speak", text:"..."}`  / `{action:"skip"}` の制御フロー。
4. **収束判断** — facilitator が議論を止揚するタイミング判定。

### 3-3. ハイパーパラメータ (初期値)

```python
max_seq_length = 2048
lora_r = 16
lora_alpha = 32
lora_dropout = 0.05
target_modules = ["q_proj", "k_proj", "v_proj", "o_proj",
                  "gate_proj", "up_proj", "down_proj"]
num_train_epochs = 3
per_device_train_batch_size = 2
gradient_accumulation_steps = 4  # effective batch = 8
learning_rate = 2e-4
lr_scheduler_type = "cosine"
warmup_ratio = 0.05
```

### 3-4. 実行コマンド

```bash
# WSL2 内で実行
cd /mnt/e/Document/Ars/Discutere
python scripts/ft/unsloth_train.py \
  --data data/ft-export/debate-YYYY-MM-DD.jsonl \
  --output models/di-debate-v1 \
  --epochs 3
```

---

## 4. Ollama 登録フェーズ

### 4-1. GGUF 変換

```bash
# WSL2 内
python -m llama_cpp.convert \
  models/di-debate-v1 \
  --outfile models/di-debate-v1.gguf \
  --outtype q4_k_m
```

### 4-2. Modelfile 作成と push

```
# models/di-debate-v1.Modelfile
FROM ./di-debate-v1.gguf

SYSTEM """
あなたは Di 議論エンジンの Gemma ベース persona です。
議論に参加するときは JSON 形式で返答してください。
"""
PARAMETER temperature 0.8
PARAMETER top_p 0.9
PARAMETER stop "<end_of_turn>"
```

```bash
ollama create di-debate:v1 -f models/di-debate-v1.Modelfile
ollama run di-debate:v1 --verbose
```

### 4-3. Di への接続

`discutere.config.json`:

```json
{
  "llm": {
    "backend": "local",
    "local": {
      "baseUrl": "http://localhost:11434/v1",
      "model": "di-debate:v1",
      "timeoutMs": 60000
    }
  }
}
```

---

## 5. 評価基準

| 指標 | 合格ライン |
|---|---|
| 役割別口調一致率 (人手判定) | ≥ 80% |
| JSON 出力パース成功率 | ≥ 95% |
| skip 適切率 (スキップが自然なターン) | ≥ 70% |
| 1 ターン生成レイテンシ (RTX3080等) | ≤ 8 秒 |

---

## 6. 反復計画

| バージョン | 変更点 |
|---|---|
| v1 | 基礎 QLoRA、全 persona 混合学習 |
| v2 | persona 別モデル or LoRA adapter 分岐 |
| v3 | facilitator の収束判断を専用 RLHF で強化 |

---

## 7. ファイル構成

```
Discutere/
├── data/
│   ├── worker-turns/        # teacher 出力 (turn + reply)
│   └── ft-export/           # JSONL 学習データ
├── models/
│   ├── di-debate-v1/        # HF format adapter
│   ├── di-debate-v1.gguf    # GGUF 量子化済み
│   └── di-debate-v1.Modelfile
└── scripts/
    ├── export-ft-data.ts    # #138: JSONL エクスポート
    ├── generate-training-data.ts  # #139: バッチ生成
    └── ft/
        └── unsloth_train.py # #140: QLoRA 学習
```

---

## 8. 依存・前提

- **Unsloth**: `pip install unsloth` (CUDA 12.1 環境)
- **Ollama**: v0.3+ (Gemma3 対応)
- **WSL2**: CUDA passthrough (NVIDIA driver ≥ 555)
- **Di headless**: `npm run headless -- --persist` が動作すること
- **`data/worker-turns/`**: 5,000 ターン以上のペアが揃っていること

---

## 9. 蒸留品質の改善計画 (2026-08-11, 精度向上方針 D) {#SPEC-GEMMA-FT-DISTILL-QUALITY}

v1 の SFT は「teacher の全出力を無選別に学習」しており、teacher の失敗ターン
(浅い相槌・論点の繰り返し・skip すべきだった発話) までコピーする。student の
精度は教師データの質で決まるため、次の 4 段で品質を上げる。

### 9-1. エクスポート時の決定的品質フィルタ (先行実装候補)

`export-ft-data.ts` に機械判定のフィルタを足す (LLM 不要・コストゼロ):

- **短文/定型除去**: 本文 < 40 文字、`{action:"skip"}` 近傍の空返答、同一 persona の
  直前ターンとの bigram Jaccard > 0.9 (繰り返し) を落とす。
- **役割リーク除去**: system prompt の指示文をそのまま復唱しているターンを落とす。
- **セッション偏り制限**: 1 セッションからの採用上限を設け、長い 1 議論への過適合を防ぐ。

### 9-2. Rejection sampling (teacher best-of-N)

`generate-training-data.ts` に `--best-of N` を追加: 同一ターンを teacher に N 回
生成させ、judge (Haiku で可: 論点新規性/具体性/persona 口調の 3 軸採点) が最高点の
1 本だけを学習データに採用する。生成コストは N 倍だが、SFT はデータ質 > 量。

### 9-3. DPO ペアの併産

9-2 の best/worst ペアをそのまま DPO (chosen/rejected) 用 JSONL に併産し、
SFT 後に DPO を 1 epoch 当てる (Unsloth 対応済み)。「悪い発話の型」を明示的に
遠ざけられるのは SFT 単独に無い利点。

### 9-4. 蒸留の評価 (卒業判定)

held-out 議題 20 件で di-debate と teacher の発話を並べ、judge が blind 勝率を出す
(`scripts/ft/` に eval を追加)。勝率 > 40% を「ローカル切替可」の目安とする。
判定の再現性のため judge prompt と採点軸は spec に固定する。

> 実行順: 9-1 (決定的・即効) → 9-2/9-3 (生成コストを伴う) → 9-4 (卒業判定)。
> 9-2 以降は teacher 生成 100 セッション級のバッチが要るため、Cc delegation の
> バックグラウンドジョブとして回すのが妥当。
