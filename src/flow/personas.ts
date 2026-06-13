/**
 * 議論フロー用ペルソナ生成 (使い捨て・永続しない)。
 *
 * FlowDirector が議論開始時に personaCount 人を生成する。
 * ロール: ファシリテーター ×1 + 残りを debater / opinion に割当。
 * 賛否 (pro/con) はターン時に決定するため、ここでは stance を持たない。
 */

import { randomUUID } from "node:crypto";

export type FlowRole = "facilitator" | "debater" | "opinion";
export type FlowStance = "pro" | "con" | "neutral" | "opinion";
export type Rng = () => number;

export interface FlowPersona {
  id: string;
  name: string;
  role: FlowRole;
  speechStyle: string;
  traits: string[];
  model: string;
  isLocal: boolean;
}

// ── 人格プール (composition.ts と同じプールを流用) ──────────────────────────

const NAME_POOL = [
  "陽介", "凛", "ハル", "美咲", "翔", "葵", "蓮", "結衣", "拓海", "彩",
  "悠真", "詩織", "大樹", "舞", "颯太", "真央", "啓介", "杏奈", "亮", "千夏",
];

const ROLE_FLAVOR: Record<FlowRole, { style: string; traits: string[] }> = {
  facilitator: {
    style: "穏やかな進行役口調",
    traits: ["中立", "俯瞰", "巻き込み上手"],
  },
  debater: {
    style: "芯のある主張口調",
    traits: ["論点を掴む", "具体例豊富", "対立を恐れない"],
  },
  opinion: {
    style: "自然な雑談口調",
    traits: ["独自の角度", "素直", "発想が柔らかい"],
  },
};

function pickName(used: Set<string>, rng: Rng): string {
  const free = NAME_POOL.filter((n) => !used.has(n));
  const pool = free.length > 0 ? free : NAME_POOL;
  const name = pool[Math.floor(rng() * pool.length)] ?? NAME_POOL[0];
  const final = used.has(name) ? `${name}${used.size}` : name;
  used.add(final);
  return final;
}

/**
 * ターン時の賛否決定。
 * - facilitator → neutral
 * - opinion → opinion (常に意見役スタンス)
 * - debater → pro / con を 50:50 でランダム
 */
export function decideStance(persona: FlowPersona, rng: Rng): FlowStance {
  if (persona.role === "facilitator") return "neutral";
  if (persona.role === "opinion") return "opinion";
  return rng() < 0.5 ? "pro" : "con";
}

export interface GeneratePersonasArgs {
  count: number;
  /** 議論メンバーの LLM モデル (既定: config から取得) */
  defaultModel: string;
  /** ローカル LLM backend かどうか */
  isLocal: boolean;
  rng: Rng;
}

/**
 * `count` 人のペルソナを生成する。
 * 1人目はファシリテーター、残りは opinion と debater に均等配分。
 */
export function generateFlowPersonas(args: GeneratePersonasArgs): FlowPersona[] {
  const { count, defaultModel, isLocal, rng } = args;
  if (count < 1) throw new Error("personaCount must be >= 1");

  const used = new Set<string>();
  const personas: FlowPersona[] = [];

  // 1 人目はファシリテーター
  personas.push({
    id: randomUUID(),
    name: pickName(used, rng),
    role: "facilitator",
    speechStyle: ROLE_FLAVOR.facilitator.style,
    traits: ROLE_FLAVOR.facilitator.traits,
    model: defaultModel,
    isLocal,
  });

  // 残りの人数を opinion と debater に 1:2 で配分
  const remaining = count - 1;
  const opinionCount = Math.max(1, Math.floor(remaining / 3));
  const debaterCount = remaining - opinionCount;

  for (let i = 0; i < opinionCount; i++) {
    personas.push({
      id: randomUUID(),
      name: pickName(used, rng),
      role: "opinion",
      speechStyle: ROLE_FLAVOR.opinion.style,
      traits: ROLE_FLAVOR.opinion.traits,
      model: defaultModel,
      isLocal,
    });
  }
  for (let i = 0; i < debaterCount; i++) {
    personas.push({
      id: randomUUID(),
      name: pickName(used, rng),
      role: "debater",
      speechStyle: ROLE_FLAVOR.debater.style,
      traits: ROLE_FLAVOR.debater.traits,
      model: defaultModel,
      isLocal,
    });
  }

  return personas;
}

/** ペルソナリストからランダムに 1 人選ぶ。 */
export function pickRandomPersona(personas: FlowPersona[], rng: Rng): FlowPersona {
  const idx = Math.floor(rng() * personas.length);
  return personas[idx];
}
