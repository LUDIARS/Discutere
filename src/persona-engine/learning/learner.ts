/**
 * Learning loop (PR-F / MISSING #3-1).
 *
 * hypothesis が integrated / rejected されたタイミングで、 提案した persona の
 * learned_notes に結果を蓄積する。 提案者 persona は rule_log の detail から
 * 逆引きする ("persona <id> proposed hypothesis <hid>" パターン)。
 */

import type { PersonasRepo } from "../db/personas-repo.js";
import type { RulesRepo } from "../db/rules-repo.js";

export type HypothesisOutcome = "integrated" | "rejected";

export interface RecordOutcomeArgs {
  personas: PersonasRepo;
  rules: RulesRepo;
  hypothesisId: string;
  outcome: HypothesisOutcome;
  /** 結果 hypothesis の statement (任意、 note に埋め込む) */
  statement?: string;
  /** test 用 — 検索範囲を増やす */
  recentLogsLimit?: number;
}

const PROPOSE_PATTERN = /persona\s+(\S+)\s+proposed\s+hypothesis\s+(\S+)/i;

export function recordHypothesisOutcome(args: RecordOutcomeArgs): {
  recorded: boolean;
  personaId?: string;
} {
  const proposerId = findProposerPersona(
    args.rules,
    args.hypothesisId,
    args.recentLogsLimit ?? 500
  );
  if (!proposerId) {
    return { recorded: false };
  }
  const note = formatNote(args.outcome, args.hypothesisId, args.statement);
  try {
    args.personas.appendLearnedNote(proposerId, note);
    return { recorded: true, personaId: proposerId };
  } catch {
    return { recorded: false };
  }
}

function findProposerPersona(
  rules: RulesRepo,
  hypothesisId: string,
  scanLimit: number
): string | null {
  const logs = rules.recentLogs(scanLimit);
  for (const log of logs) {
    if (!log.detail) continue;
    const m = PROPOSE_PATTERN.exec(log.detail);
    if (m && m[2] === hypothesisId) {
      return m[1];
    }
  }
  return null;
}

function formatNote(
  outcome: HypothesisOutcome,
  hypothesisId: string,
  statement?: string
): string {
  const short = hypothesisId.slice(0, 8);
  const stmt = statement ? ` — "${statement.slice(0, 60)}"` : "";
  if (outcome === "integrated") {
    return `✅ integrated: hyp:${short}${stmt}`;
  }
  return `❌ rejected: hyp:${short}${stmt}`;
}
