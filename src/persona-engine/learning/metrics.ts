/**
 * Persona / Rule 指標 (PR-F / MISSING #3-2 + #3-3).
 *
 * rule_log + persona.learned_notes から「採用率」 を算出。
 * Discatier の hypothesis status を引く必要があるので、 status 解決関数を inject。
 */

import type { PersonasRepo } from "../db/personas-repo.js";
import type { RulesRepo } from "../db/rules-repo.js";

export interface PersonaMetric {
  id: string;
  name: string;
  display_name: string;
  /** 提案 hypothesis 総数 */
  proposed: number;
  /** integrated になった数 */
  integrated: number;
  /** rejected になった数 */
  rejected: number;
  /** integrated 率 (proposed > 0 のとき 0-1、 0 件なら null) */
  acceptanceRate: number | null;
}

export interface RuleMetric {
  id: string;
  fires: number;
  skips: number;
  errors: number;
}

export interface HypothesisStatusResolver {
  (hypothesisId: string): "integrated" | "rejected" | "pending" | "unknown";
}

const PROPOSE_PATTERN = /persona\s+(\S+)\s+proposed\s+hypothesis\s+(\S+)/i;

export function personaMetrics(
  personas: PersonasRepo,
  rules: RulesRepo,
  resolveStatus: HypothesisStatusResolver,
  scanLimit = 1000
): PersonaMetric[] {
  const all = personas.list();
  const byPersona = new Map<string, { proposed: number; integrated: number; rejected: number }>();
  for (const p of all) {
    byPersona.set(p.id, { proposed: 0, integrated: 0, rejected: 0 });
  }
  const logs = rules.recentLogs(scanLimit);
  for (const log of logs) {
    if (log.action !== "fire" || !log.detail) continue;
    const m = PROPOSE_PATTERN.exec(log.detail);
    if (!m) continue;
    const personaId = m[1];
    const hypothesisId = m[2];
    const bucket = byPersona.get(personaId);
    if (!bucket) continue;
    bucket.proposed += 1;
    const status = resolveStatus(hypothesisId);
    if (status === "integrated") bucket.integrated += 1;
    else if (status === "rejected") bucket.rejected += 1;
  }
  return all.map((p) => {
    const b = byPersona.get(p.id) ?? { proposed: 0, integrated: 0, rejected: 0 };
    return {
      id: p.id,
      name: p.name,
      display_name: p.display_name,
      proposed: b.proposed,
      integrated: b.integrated,
      rejected: b.rejected,
      acceptanceRate: b.proposed > 0 ? b.integrated / b.proposed : null,
    };
  });
}

export function ruleMetrics(rules: RulesRepo, scanLimit = 1000): RuleMetric[] {
  const logs = rules.recentLogs(scanLimit);
  const byRule = new Map<string, RuleMetric>();
  for (const log of logs) {
    if (!log.rule_id) continue;
    let m = byRule.get(log.rule_id);
    if (!m) {
      m = { id: log.rule_id, fires: 0, skips: 0, errors: 0 };
      byRule.set(log.rule_id, m);
    }
    if (log.action === "fire") m.fires += 1;
    else if (log.action === "skip") m.skips += 1;
    else if (log.action === "error") m.errors += 1;
  }
  return [...byRule.values()].sort((a, b) => b.fires - a.fires);
}
