/**
 * ⑦ 破綻検出・修正。
 *
 * 機械判定可能な破綻 (経済収支・重複) は即時検出。
 * 主観的破綻 (面白さ) は LLM 評価 + designGap 化 (収束ループ上限付き)。
 *
 * spec/plan/RESTRUCTURE.md §2-⑦ / §4-E。
 */

import type {
  EconomyPhase0,
  Step6Output,
  Step7Output,
  IncoherenceIssue,
  CombinationCandidate,
} from "./types.js";
import type { LlmAdapter } from "./llm-adapter.js";

const MAX_ITERATIONS = 3;

// ── 機械判定 ──────────────────────────────────────────────────────────────────

function detectEconomyImbalance(
  economy: EconomyPhase0,
  candidates: CombinationCandidate[],
): IncoherenceIssue[] {
  const issues: IncoherenceIssue[] = [];
  const resourceBalance = new Map<string, number>(); // positive = surplus

  for (const edge of economy.edges) {
    const delta = edge.type === "produces" ? 1 : -1;
    resourceBalance.set(edge.targetResource, (resourceBalance.get(edge.targetResource) ?? 0) + delta);
  }

  // 候補メカニクスも仮接続して収支確認
  for (const c of candidates) {
    if (c.mechanic.description?.toLowerCase().includes("produc")) {
      // heuristic: description が "produc" を含むなら produce 側と見なす
      resourceBalance.set("hypothetical_resource", (resourceBalance.get("hypothetical_resource") ?? 0) + 1);
    }
  }

  for (const [resource, balance] of resourceBalance) {
    if (balance > 3) {
      issues.push({
        type: "economy_imbalance",
        description: `Resource "${resource}" has excess producers (balance +${balance}). Inflationary risk.`,
        affectedMechanics: economy.edges
          .filter((e) => e.targetResource === resource && e.type === "produces")
          .map((e) => e.sourceMechanic),
        suggestedFix: `Add a sink mechanic that consumes "${resource}" or reduce producer count.`,
      });
    } else if (balance < -2) {
      issues.push({
        type: "economy_imbalance",
        description: `Resource "${resource}" has excess consumers (balance ${balance}). Depletion/deadlock risk.`,
        affectedMechanics: economy.edges
          .filter((e) => e.targetResource === resource && e.type === "consumes")
          .map((e) => e.sourceMechanic),
        suggestedFix: `Add a source mechanic that produces "${resource}" or reduce consumer dependency.`,
      });
    }
  }

  return issues;
}

function detectMechanicConflicts(
  candidates: CombinationCandidate[],
  existingMechanicNames: Set<string>,
): IncoherenceIssue[] {
  const issues: IncoherenceIssue[] = [];
  const candidateNames = new Set<string>();

  for (const c of candidates) {
    const name = c.mechanic.name.toLowerCase();
    if (existingMechanicNames.has(name)) {
      issues.push({
        type: "mechanic_conflict",
        description: `Candidate "${c.mechanic.name}" from ${c.sourceGame} duplicates an existing mechanic.`,
        affectedMechanics: [c.mechanic.name],
        suggestedFix: `Skip "${c.mechanic.name}" or merge it with the existing equivalent.`,
      });
    }
    if (candidateNames.has(name)) {
      issues.push({
        type: "mechanic_conflict",
        description: `Duplicate candidate "${c.mechanic.name}" from multiple sources.`,
        affectedMechanics: [c.mechanic.name],
        suggestedFix: `Keep only the highest-scoring version of "${c.mechanic.name}".`,
      });
    }
    candidateNames.add(name);
  }

  return issues;
}

// ── 収束ループ ────────────────────────────────────────────────────────────────

export async function fixIncoherence(
  combination: Step6Output,
  economy: EconomyPhase0,
  existingMechanicNames: Set<string>,
  llm: LlmAdapter,
): Promise<Step7Output> {
  let candidates = [...combination.candidates];
  let iterationCount = 0;
  const allIssues: IncoherenceIssue[] = [];

  for (let i = 0; i < MAX_ITERATIONS; i++) {
    iterationCount++;

    // 機械判定
    const mechanicalIssues = [
      ...detectEconomyImbalance(economy, candidates),
      ...detectMechanicConflicts(candidates, existingMechanicNames),
    ];

    // LLM 判定 (主観的破綻)
    const llmIssues = await llm.detectIncoherence(candidates, economy);

    const roundIssues = [...mechanicalIssues, ...llmIssues];
    allIssues.push(...roundIssues);

    if (roundIssues.length === 0) break;

    // 機械的修正: conflict / 経済破綻で名指しされた候補を除去
    const problematicNames = new Set(
      roundIssues.flatMap((issue) => issue.affectedMechanics.map((n) => n.toLowerCase())),
    );
    candidates = candidates.filter(
      (c) => !problematicNames.has(c.mechanic.name.toLowerCase()),
    );

    if (candidates.length === 0) break;
  }

  // 確定 hypothesis テキスト
  const resolvedHypotheses = candidates.map(
    (c) => `採用: ${c.mechanic.name} (出典: ${c.provenance}) — ${c.rationale}`,
  );

  return {
    issues: allIssues,
    iterationCount,
    resolvedHypotheses,
    converged: allIssues.length === 0 || candidates.length > 0,
  };
}
