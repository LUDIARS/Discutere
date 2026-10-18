/**
 * 未来予測フェーズの話者割当 (pure)。
 *
 * 本議論の発話から話者を引き継ぎ、改革派 (賛同) / 保守派 (否定) に振り分ける。
 * エンジン (rounds / dialectic) に依らず FlowUtteranceRecord だけから決めるので、
 * dispatch の後段でどちらのエンジンの結果にも同じ規則で使える。
 *
 *   - 陣営は本議論の賛否 (pro / con) とは独立に、初登場順に交互に割り当てる
 *     (本議論の命題への賛否と、提案された一手への賛否は別の問いなので同一視しない。
 *      2026-10-18 Fable 評価)。
 *   - 各話者には本議論での自分の発言 (直近 OWN_STATEMENT_LIMIT 件) を持たせ、視点と口調の一貫性に使う
 *   - 進行役 (facilitator) は陣営に入れず、提案と予測を担う
 */

import type { FlowUtteranceRecord } from "../director.js";
import type { CampStage, ForesightCamp } from "./prompts.js";

export interface ForesightSpeaker {
  personaId: string;
  personaName: string;
  role: string;
  camp: ForesightCamp;
  /** 本議論での自分の発言 (新しい順ではなく発言順、各 OWN_STATEMENT_CHARS 字まで)。 */
  ownStatements: string[];
}

export interface ForesightCast {
  facilitator: { personaId: string; personaName: string; role: string };
  speakers: ForesightSpeaker[];
}

/** 話者に持たせる本議論の発言の件数と 1 件の字数。 */
const OWN_STATEMENT_LIMIT = 2;
const OWN_STATEMENT_CHARS = 240;

/** 1 往復 = 改革派 1 回 + 保守派 1 回。 */
const TURNS_PER_EXCHANGE = 2;
const STAGES: readonly CampStage[] = ["conditions", "refutation", "verification"];

/** 進行役が本議論に居ないときの代役。 */
const FALLBACK_FACILITATOR = { personaId: "foresight-facilitator", personaName: "進行役", role: "facilitator" };

function clip(text: string): string {
  const chars = Array.from(text.trim());
  return chars.length > OWN_STATEMENT_CHARS ? `${chars.slice(0, OWN_STATEMENT_CHARS).join("")}…` : chars.join("");
}

/**
 * 本議論の発話から未来予測フェーズのキャストを作る。
 * 進行役以外の話者が 2 人未満 (改革派・保守派のどちらかが 0 人) なら null (フェーズを行わない)。
 */
export function buildForesightCast(utterances: readonly FlowUtteranceRecord[]): ForesightCast | null {
  const valid = utterances.filter((u) => !u.isError);
  const facilitatorRecord = valid.find((u) => u.role === "facilitator");
  const facilitator = facilitatorRecord
    ? { personaId: facilitatorRecord.personaId, personaName: facilitatorRecord.personaName, role: "facilitator" }
    : FALLBACK_FACILITATOR;

  const order: FlowUtteranceRecord[] = [];
  const statements = new Map<string, string[]>();
  for (const u of valid) {
    if (u.role === "facilitator") continue;
    if (!statements.has(u.personaId)) {
      order.push(u);
      statements.set(u.personaId, []);
    }
    statements.get(u.personaId)!.push(u.text);
  }
  if (order.length < 2) return null;

  const speakers: ForesightSpeaker[] = order.map((u, i) => ({
    personaId: u.personaId,
    personaName: u.personaName,
    role: u.role,
    camp: i % 2 === 0 ? "reform" : "conservative",
    ownStatements: statements.get(u.personaId)!.slice(-OWN_STATEMENT_LIMIT).map(clip),
  }));
  return { facilitator, speakers };
}

/** 発言回数を往復単位 (偶数、最低 1 往復) に揃える。turns=1 でも保守派が必ず話す。 */
export function normalizeCampTurns(turns: number): number {
  const n = Number.isFinite(turns) ? Math.floor(turns) : TURNS_PER_EXCHANGE;
  return Math.max(TURNS_PER_EXCHANGE, Math.ceil(n / TURNS_PER_EXCHANGE) * TURNS_PER_EXCHANGE);
}

/** i 番目のターンの段階 (往復ごとに 成立条件 → 反証 → 確かめ方、4 往復目以降は確かめ方のまま)。 */
export function campStageAt(turnIndex: number): CampStage {
  const exchange = Math.floor(turnIndex / TURNS_PER_EXCHANGE);
  return STAGES[Math.min(exchange, STAGES.length - 1)];
}

/**
 * 議論ターンの話者順。改革派と保守派を交互に並べ、各陣営の中は巡回する。
 * 改革派から始める (提案を擁護する側が先に口火を切る)。回数は normalizeCampTurns で往復単位に揃える。
 */
export function buildCampTurnOrder(speakers: readonly ForesightSpeaker[], turns: number): ForesightSpeaker[] {
  const reform = speakers.filter((s) => s.camp === "reform");
  const conservative = speakers.filter((s) => s.camp === "conservative");
  if (reform.length === 0 || conservative.length === 0) return [];
  const order: ForesightSpeaker[] = [];
  const total = normalizeCampTurns(turns);
  for (let i = 0; i < total; i++) {
    const side = i % 2 === 0 ? reform : conservative;
    order.push(side[Math.floor(i / 2) % side.length]);
  }
  return order;
}
