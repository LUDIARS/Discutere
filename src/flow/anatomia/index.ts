/**
 * Anatomia 連携の入口 (取得 + 精製のオーケストレーション)。
 *
 *   getAnatomiaMechanics(source) = fetchAnatomiaDomains (CLI) → refineDomainsToMechanics (LLM)
 *
 * 議論前にこれを呼ぶと、対象リポのドメイン構造が Di の事前情報メカニクスになる。
 * 配線は `paper-review.buildPaperDraft` (extraMechanics) と `/api/flow/start`。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { MechanicSummary } from "../investigate.js";
import { fetchAnatomiaDomains, type AnatomiaCliRunner } from "./client.js";
import { refineDomainsToMechanics } from "./refine.js";
import type { AnatomiaSource } from "./types.js";

export type { AnatomiaSource, AnatomiaDomainCard } from "./types.js";
export { fetchAnatomiaDomains, defaultAnatomiaCliRunner } from "./client.js";
export { refineDomainsToMechanics, fallbackDomainsToMechanics } from "./refine.js";

export interface GetAnatomiaMechanicsDeps {
  /** Anatomia bin/anatomia.mjs の絶対パス (空なら sibling 解決)。 */
  binPath?: string;
  autoDraft?: boolean;
  timeoutMs?: number;
  /** 精製に使う LLM (未指定なら決定論フォールバック)。 */
  llm?: LLMClient;
  /** 精製モデル。 */
  refineModel?: string;
  /** 議題 (精製の文脈手掛かり)。 */
  theme?: string;
  runner?: AnatomiaCliRunner;
  warn?: (msg: string) => void;
}

/**
 * 対象 (project / repoPath) のドメインを Anatomia から取得し、事前情報メカニクスに精製する。
 * 取得失敗 (bin 不在 / CLI 非 0 / JSON 不正) は fetchAnatomiaDomains が throw する (fail-fast)。
 */
export async function getAnatomiaMechanics(
  source: AnatomiaSource,
  deps: GetAnatomiaMechanicsDeps = {}
): Promise<MechanicSummary[]> {
  const cards = await fetchAnatomiaDomains(source, {
    binPath: deps.binPath,
    autoDraft: deps.autoDraft,
    timeoutMs: deps.timeoutMs,
    runner: deps.runner,
    warn: deps.warn,
  });
  return refineDomainsToMechanics(cards, {
    llm: deps.llm,
    model: deps.refineModel,
    theme: deps.theme,
    warn: deps.warn,
  });
}

/** リクエスト由来の文字列ペアから AnatomiaSource を組み立てる (project 優先・両空は null)。 */
export function resolveAnatomiaSource(
  project?: string,
  repoPath?: string
): AnatomiaSource | null {
  const p = project?.trim();
  if (p) return { project: p };
  const r = repoPath?.trim();
  if (r) return { repoPath: r };
  return null;
}
