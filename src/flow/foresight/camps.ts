/**
 * 未来予測フェーズの話者割当 (pure)。
 *
 * 本議論の発話から話者を引き継ぎ、改革派 (賛同) / 保守派 (否定) に振り分ける。
 * エンジン (rounds / dialectic) に依らず FlowUtteranceRecord だけから決めるので、
 * dispatch の後段でどちらのエンジンの結果にも同じ規則で使える。
 *
 *   - 賛成派 (pro) → 改革派、反対派 (con) → 保守派
 *   - 意見屋など陣営の無い話者は、人数の少ない陣営へ順に足す (両陣営を埋める)
 *   - 進行役 (facilitator) は陣営に入れず、提案と予測を担う
 */

import type { FlowUtteranceRecord } from "../director.js";
import type { ForesightCamp } from "./prompts.js";

export interface ForesightSpeaker {
  personaId: string;
  personaName: string;
  role: string;
  camp: ForesightCamp;
}

export interface ForesightCast {
  facilitator: { personaId: string; personaName: string; role: string };
  speakers: ForesightSpeaker[];
}

/** 進行役が本議論に居ないときの代役。 */
const FALLBACK_FACILITATOR = { personaId: "foresight-facilitator", personaName: "進行役", role: "facilitator" };

/**
 * 本議論の発話から未来予測フェーズのキャストを作る。
 * 改革派・保守派のどちらかが 0 人になる (話者が 2 人未満) なら null (フェーズを行わない)。
 */
export function buildForesightCast(utterances: readonly FlowUtteranceRecord[]): ForesightCast | null {
  const valid = utterances.filter((u) => !u.isError);
  const facilitatorRecord = valid.find((u) => u.role === "facilitator");
  const facilitator = facilitatorRecord
    ? { personaId: facilitatorRecord.personaId, personaName: facilitatorRecord.personaName, role: "facilitator" }
    : FALLBACK_FACILITATOR;

  const seen = new Map<string, FlowUtteranceRecord>();
  for (const u of valid) {
    if (u.role === "facilitator" || seen.has(u.personaId)) continue;
    seen.set(u.personaId, u);
  }

  const speakers: ForesightSpeaker[] = [];
  const unassigned: FlowUtteranceRecord[] = [];
  for (const u of seen.values()) {
    const camp: ForesightCamp | null = u.stance === "pro" ? "reform" : u.stance === "con" ? "conservative" : null;
    if (camp) speakers.push({ personaId: u.personaId, personaName: u.personaName, role: u.role, camp });
    else unassigned.push(u);
  }
  for (const u of unassigned) {
    const reform = speakers.filter((s) => s.camp === "reform").length;
    const conservative = speakers.length - reform;
    const camp: ForesightCamp = reform <= conservative ? "reform" : "conservative";
    speakers.push({ personaId: u.personaId, personaName: u.personaName, role: u.role, camp });
  }

  const hasBoth = speakers.some((s) => s.camp === "reform") && speakers.some((s) => s.camp === "conservative");
  return hasBoth ? { facilitator, speakers } : null;
}

/**
 * 議論ターンの話者順。改革派と保守派を交互に並べ、各陣営の中は巡回する。
 * 改革派から始める (提案を擁護する側が先に口火を切る)。
 */
export function buildCampTurnOrder(speakers: readonly ForesightSpeaker[], turns: number): ForesightSpeaker[] {
  const reform = speakers.filter((s) => s.camp === "reform");
  const conservative = speakers.filter((s) => s.camp === "conservative");
  if (reform.length === 0 || conservative.length === 0) return [];
  const order: ForesightSpeaker[] = [];
  for (let i = 0; i < turns; i++) {
    const side = i % 2 === 0 ? reform : conservative;
    order.push(side[Math.floor(i / 2) % side.length]);
  }
  return order;
}
