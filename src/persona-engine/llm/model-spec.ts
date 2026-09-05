/**
 * モデル指定子 (model spec) — `"<model>@<effort>"` の 1 文字列で
 * モデル ID と推論の深さ (effort) を運ぶ。
 *
 * 議論フローはペルソナごとに `model` 文字列 1 つしか持ち回らないため、
 * effort を別フィールドで全経路に配線する代わりに model 文字列へ同梱する。
 * 各 LLM client (claude-cli / anthropic / codex-cli) が呼び出し直前に
 * `parseModelSpec` で分解して CLI/API の引数へ落とす。
 *
 *   "claude-opus-5@xhigh"  → { model: "claude-opus-5", effort: "xhigh" }
 *   "gpt-5.6-sol@medium"   → { model: "gpt-5.6-sol",   effort: "medium" }
 *   "claude-fable-5-1"     → { model: "claude-fable-5-1" }  (effort 未指定 = 既定)
 *
 * effort の語彙は provider 側の受け入れ値に合わせる (claude: low/medium/high/xhigh/max、
 * codex: minimal/low/medium/high/xhigh)。"mid" は medium の別名として吸収する。
 */

export interface ModelSpec {
  model: string;
  effort?: string;
}

export type ModelProvider = "claude" | "codex";

const EFFORT_ALIASES: Record<string, string> = {
  mid: "medium",
  med: "medium",
};

const EFFORTS_BY_PROVIDER: Record<ModelProvider, ReadonlySet<string>> = {
  claude: new Set(["low", "medium", "high", "xhigh", "max"]),
  codex: new Set(["minimal", "low", "medium", "high", "xhigh"]),
};

// CLI 引数は Windows で shell 経由になる場合があるため、モデル ID をデータとして扱える
// 保守的な文字集合に限定する。現行 provider の ID / ローカルモデル指定を包含する。
const MODEL_ID_PATTERN = /^[a-z0-9][a-z0-9._:/-]*$/i;

export function parseModelSpec(spec: string | undefined): ModelSpec {
  if (!spec) return { model: "" };
  const at = spec.lastIndexOf("@");
  if (at <= 0) return { model: spec.trim() };
  const model = spec.slice(0, at).trim();
  const rawEffort = spec.slice(at + 1).trim().toLowerCase();
  if (!rawEffort) return { model };
  return { model, effort: EFFORT_ALIASES[rawEffort] ?? rawEffort };
}

/** @implements SPEC-FLOW-MODEL-ROSTER — provider 境界へ渡す model spec を fail-fast 検証する。 */
export function validateModelSpec(spec: ModelSpec, provider: ModelProvider): string | null {
  if (!spec.model) return "model 未指定";
  if (!MODEL_ID_PATTERN.test(spec.model)) return "model に使用できない文字が含まれています";
  if (spec.effort && !EFFORTS_BY_PROVIDER[provider].has(spec.effort)) {
    return `${provider} で未対応の effort です`;
  }
  return null;
}

/** model spec が Codex (GPT 系) を指すか。`gpt-` 接頭辞で判定する。 */
export function isCodexModel(spec: string | undefined): boolean {
  return /^gpt-/i.test(parseModelSpec(spec).model);
}

/** spec の model 部分だけを返す (pricing / ログ用)。 */
export function modelOfSpec(spec: string | undefined): string | undefined {
  const m = parseModelSpec(spec).model;
  return m || undefined;
}
