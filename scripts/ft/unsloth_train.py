"""
Di FT — Gemma-3-12b QLoRA fine-tuning (spec/feature/gemma-ft.md task #140).

WSL2 / Linux (CUDA 12.1+) 上で実行する。Windows では動作しない。

使い方:
  python scripts/ft/unsloth_train.py \
    --data data/ft-export/debate-2026-06-12.jsonl \
    --output models/di-debate-v1 \
    --epochs 3

依存:
  pip install "unsloth[colab-new] @ git+https://github.com/unslothai/unsloth.git"
  pip install trl transformers accelerate bitsandbytes
"""

import argparse
import json
import sys
from pathlib import Path

# ── 依存チェック ──────────────────────────────────────────────────────────────
try:
    from unsloth import FastLanguageModel
    from trl import SFTTrainer, SFTConfig
    from datasets import Dataset
    import torch
except ImportError as e:
    print(f"[error] 依存ライブラリが不足しています: {e}", file=sys.stderr)
    print("  pip install 'unsloth[colab-new] @ git+https://github.com/unslothai/unsloth.git'", file=sys.stderr)
    print("  pip install trl transformers accelerate bitsandbytes datasets", file=sys.stderr)
    sys.exit(1)

# ── 定数 ──────────────────────────────────────────────────────────────────────
BASE_MODEL = "unsloth/gemma-3-12b-it-bnb-4bit"
MAX_SEQ_LENGTH = 2048

LORA_R = 16
LORA_ALPHA = 32
LORA_DROPOUT = 0.05
TARGET_MODULES = [
    "q_proj", "k_proj", "v_proj", "o_proj",
    "gate_proj", "up_proj", "down_proj",
]

# ── データ ────────────────────────────────────────────────────────────────────
def load_jsonl(path: Path) -> list[dict]:
    samples = []
    with open(path, "r", encoding="utf-8") as f:
        for i, line in enumerate(f):
            line = line.strip()
            if not line:
                continue
            try:
                samples.append(json.loads(line))
            except json.JSONDecodeError as e:
                print(f"[warn] line {i+1}: JSON パースエラー — スキップ ({e})", file=sys.stderr)
    return samples


def build_chat_text(sample: dict) -> str | None:
    """OpenAI chat JSONL の messages を Gemma instruction template に変換する。"""
    messages = sample.get("messages", [])
    if not messages:
        return None

    # Gemma instruction format
    # <start_of_turn>user\n{system}\n\n{user}<end_of_turn>\n<start_of_turn>model\n{assistant}<end_of_turn>
    system_content = ""
    user_content = ""
    assistant_content = ""

    for msg in messages:
        role = msg.get("role", "")
        content = (msg.get("content") or "").strip()
        if role == "system":
            system_content = content
        elif role == "user":
            user_content = content
        elif role == "assistant":
            assistant_content = content

    if not user_content or not assistant_content:
        return None

    user_part = f"{system_content}\n\n{user_content}" if system_content else user_content
    return (
        f"<start_of_turn>user\n{user_part}<end_of_turn>\n"
        f"<start_of_turn>model\n{assistant_content}<end_of_turn>"
    )


def prepare_dataset(jsonl_path: Path) -> Dataset:
    raw = load_jsonl(jsonl_path)
    print(f"[info] JSONL 読込: {len(raw)} レコード")

    texts = []
    skipped = 0
    for s in raw:
        text = build_chat_text(s)
        if text:
            texts.append({"text": text})
        else:
            skipped += 1

    print(f"[info] 有効サンプル: {len(texts)} / スキップ: {skipped}")
    if not texts:
        print("[error] 有効サンプルが 0 件です。JSONL の形式を確認してください。", file=sys.stderr)
        sys.exit(1)

    return Dataset.from_list(texts)


# ── 学習 ──────────────────────────────────────────────────────────────────────
def train(
    data_path: Path,
    output_dir: Path,
    epochs: int,
    batch_size: int,
    grad_accum: int,
    lr: float,
) -> None:
    print(f"[info] ベースモデルをロード: {BASE_MODEL}")
    model, tokenizer = FastLanguageModel.from_pretrained(
        model_name=BASE_MODEL,
        max_seq_length=MAX_SEQ_LENGTH,
        dtype=None,   # auto (bfloat16 on Ampere+)
        load_in_4bit=True,
    )

    print("[info] LoRA アダプタを設定")
    model = FastLanguageModel.get_peft_model(
        model,
        r=LORA_R,
        target_modules=TARGET_MODULES,
        lora_alpha=LORA_ALPHA,
        lora_dropout=LORA_DROPOUT,
        bias="none",
        use_gradient_checkpointing="unsloth",
        random_state=42,
        use_rslora=False,
        loftq_config=None,
    )

    dataset = prepare_dataset(data_path)

    training_args = SFTConfig(
        output_dir=str(output_dir),
        num_train_epochs=epochs,
        per_device_train_batch_size=batch_size,
        gradient_accumulation_steps=grad_accum,
        learning_rate=lr,
        lr_scheduler_type="cosine",
        warmup_ratio=0.05,
        weight_decay=0.01,
        fp16=not torch.cuda.is_bf16_supported(),
        bf16=torch.cuda.is_bf16_supported(),
        logging_steps=10,
        save_strategy="epoch",
        save_total_limit=2,
        report_to="none",
        max_seq_length=MAX_SEQ_LENGTH,
        dataset_text_field="text",
        packing=False,
    )

    trainer = SFTTrainer(
        model=model,
        tokenizer=tokenizer,
        train_dataset=dataset,
        args=training_args,
    )

    print("[info] 学習開始")
    trainer_stats = trainer.train()
    print(f"[info] 学習完了: {trainer_stats.metrics}")

    print(f"[info] アダプタを保存: {output_dir}")
    model.save_pretrained(str(output_dir))
    tokenizer.save_pretrained(str(output_dir))

    # GGUF 変換の案内
    print()
    print("次のステップ (GGUF 変換 + Ollama 登録):")
    print(f"  python -m llama_cpp.convert {output_dir} --outfile {output_dir}.gguf --outtype q4_k_m")
    print(f"  ollama create di-debate:v1 -f models/di-debate-v1.Modelfile")
    print("  ollama run di-debate:v1")


# ── エントリポイント ──────────────────────────────────────────────────────────
def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Di FT — Gemma-3-12b QLoRA")
    p.add_argument("--data", required=True, type=Path, help="JSONL 学習データ (ft:export 出力)")
    p.add_argument("--output", default=Path("models/di-debate-v1"), type=Path, help="出力ディレクトリ")
    p.add_argument("--epochs", type=int, default=3, help="エポック数 (デフォルト 3)")
    p.add_argument("--batch-size", type=int, default=2, help="バッチサイズ (デフォルト 2)")
    p.add_argument("--grad-accum", type=int, default=4, help="勾配蓄積ステップ数 (デフォルト 4)")
    p.add_argument("--lr", type=float, default=2e-4, help="学習率 (デフォルト 2e-4)")
    return p.parse_args()


def main() -> None:
    args = parse_args()

    if not args.data.exists():
        print(f"[error] データファイルが見つかりません: {args.data}", file=sys.stderr)
        sys.exit(1)

    args.output.mkdir(parents=True, exist_ok=True)

    train(
        data_path=args.data,
        output_dir=args.output,
        epochs=args.epochs,
        batch_size=args.batch_size,
        grad_accum=args.grad_accum,
        lr=args.lr,
    )


if __name__ == "__main__":
    main()
