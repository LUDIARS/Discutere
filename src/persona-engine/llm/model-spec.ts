/**
 * モデル指定子 (model spec) — `"<model>@<effort>"` の 1 文字列で
 * モデル役割名または ID と推論の深さ (effort) を運ぶ。
 *
 * 議論フローはペルソナごとに `model` 文字列 1 つしか持ち回らないため、
 * effort を別フィールドで全経路に配線する代わりに model 文字列へ同梱する。
 * 各 LLM client (claude-cli / anthropic / codex-cli) が呼び出し直前に
 * `parseModelSpec` で分解して CLI/API の引数へ落とす。
 *
 *   "opus@xhigh"  → { model: "opus", effort: "xhigh" }
 *   "sol@medium"  → { model: "sol",  effort: "medium" }
 *   "fable"       → { model: "fable" }  (effort 未指定 = 既定)
 *
 * effort の語彙は provider 側の受け入れ値に合わせる (claude: low/medium/high/xhigh/max、
 * codex: minimal/low/medium/high/xhigh)。"mid" は medium の別名として吸収する。
 */

import { MODEL_ROLES, resolveModel } from "@ludiars/one-shot";
import { contract } from '../../flow/contracts/runtime.js'; /* augur-inject:import:a4caaed6 */
import augurContract_ea3cb2b3 from '../../flow/contracts/anthropic-model-role.contract.js'; /* augur-inject:contract-predicate:6929b81d */
import augurContract_73d6b115 from '../../flow/contracts/codex-model-role.contract.js'; /* augur-inject:contract-predicate:4ce94a17 */

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

/** model spec が Codex の役割名か Codex 用の明示 ID を指すか。 */
export function isCodexModel(spec: string | undefined): boolean {
  const { model } = parseModelSpec(spec);
  return MODEL_ROLES.codex.some((role) => role === model) || /^(?:gpt|codex)-/i.test(model);
}
// @ts-expect-error augur-inject
isCodexModel = contract(isCodexModel, { ...augurContract_73d6b115, contractId: 'C-3', mode: 'observe', sample: 1, where: 'src/persona-engine/llm/model-spec.ts:64', rule: 'contract-wrap', id: '73d6b115' }); /* augur-inject:contract-wrap:73d6b115 */

/** Anthropic Messages API には Claude Code 専用の短い別名を送れない。 */
export function resolveAnthropicModel(model: string): string {
  const resolved = resolveModel(model, "claude");
  if (!resolved.startsWith("claude-")) throw new Error(`model_role_unresolved:${model}`);
  return resolved;
}
// @ts-expect-error augur-inject
resolveAnthropicModel = contract(resolveAnthropicModel, { ...augurContract_ea3cb2b3, contractId: 'C-4', mode: 'observe', sample: 1, where: 'src/persona-engine/llm/model-spec.ts:68', rule: 'contract-wrap', id: 'ea3cb2b3' }); /* augur-inject:contract-wrap:ea3cb2b3 */

/** spec の model 部分だけを返す (pricing / ログ用)。 */
export function modelOfSpec(spec: string | undefined): string | undefined {
  const m = parseModelSpec(spec).model;
  return m || undefined;
}
