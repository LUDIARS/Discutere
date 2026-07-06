import { generateFlowPersonas, type FlowPersona, type FlowRole, type FlowStance, type Rng } from "./personas.js";
import { findPoolPersona, toFlowPersona } from "./persona-pool.js";

export interface BuildFlowCastArgs {
  count: number;
  defaultModel: string;
  isLocal: boolean;
  rng: Rng;
  personaIds?: readonly string[];
  warn?: (msg: string) => void;
}

function normalizePersonaIds(personaIds: readonly string[] | undefined): string[] {
  const seen = new Set<string>();
  const out: string[] = [];
  for (const raw of personaIds ?? []) {
    const id = raw.trim();
    if (!id || seen.has(id)) continue;
    seen.add(id);
    out.push(id);
  }
  return out;
}

function specifiedRole(role: FlowRole): FlowRole {
  return role === "facilitator" ? "opinion" : role;
}

function specifiedStance(role: FlowRole, debaterIndex: number): FlowStance | undefined {
  if (role !== "debater") return undefined;
  return debaterIndex % 2 === 0 ? "pro" : "con";
}

export function buildFlowCast(args: BuildFlowCastArgs): FlowPersona[] {
  const { count, defaultModel, isLocal, rng, warn = () => {} } = args;
  const generated = generateFlowPersonas({ count, defaultModel, isLocal, rng });
  const facilitator = generated[0];
  const specified: FlowPersona[] = [];
  const seenPoolIds = new Set<string>();
  let debaterIndex = 0;

  for (const idOrName of normalizePersonaIds(args.personaIds)) {
    const poolPersona = findPoolPersona(idOrName);
    if (!poolPersona) {
      warn(`生成済みペルソナが見つかりません: ${idOrName}`);
      continue;
    }
    if (seenPoolIds.has(poolPersona.id)) continue;
    seenPoolIds.add(poolPersona.id);

    const role = specifiedRole(poolPersona.role);
    const stance = specifiedStance(role, debaterIndex);
    if (role === "debater") debaterIndex += 1;
    specified.push(toFlowPersona(poolPersona, { role, defaultModel, isLocal, ...(stance ? { stance } : {}) }));
  }

  const targetCount = Math.max(count, 1 + specified.length);
  const usedIds = new Set<string>([facilitator.id, ...specified.map((p) => p.id)]);
  const usedNames = new Set<string>([facilitator.name, ...specified.map((p) => p.name)]);
  const fillers: FlowPersona[] = [];

  if (1 + specified.length < targetCount) {
    for (const persona of generated.slice(1)) {
      if (usedIds.has(persona.id) || usedNames.has(persona.name)) continue;
      fillers.push(persona);
      usedIds.add(persona.id);
      usedNames.add(persona.name);
      if (1 + specified.length + fillers.length >= targetCount) break;
    }
  }

  return [facilitator, ...specified, ...fillers];
}
