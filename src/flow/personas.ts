/**
 * 議論フロー用ペルソナ生成 (使い捨て・キャストは flow_session_persona に永続)。
 *
 * FlowDirector が議論開始時に personaCount 人を生成する。
 * ロール: ファシリテーター ×1 + 残りを debater / opinion に割当。
 * 賛否 (pro/con) は **セッション開始時に固定**する (respec 01, A1)。
 * debater は pro/con に均等割 (奇数なら rng で余り 1 を振る)。
 * ターンごとの再抽選 (旧 decideStance) は廃止 — 立場の一貫性が議論の積み上げの前提。
 */

import { randomUUID } from "node:crypto";

export type FlowRole = "facilitator" | "debater" | "opinion";
export type FlowStance = "pro" | "con" | "neutral" | "opinion";
export type Rng = () => number;

/**
 * 憑依 (B): データ由来のプールペルソナを「人物像」として演じている状態。
 * casual な flow ペルソナ名は保持したまま、この人物像になりきって発言する。
 * 露出面では `名前 (ロール/label)` と表示し、prompt には descriptor を注入する。
 */
export interface PersonaPossession {
  /** 憑依対象の露出名 (論者#xxxxxx 等)。表示名の括弧内に出す。 */
  label: string;
  /** prompt に注入する人物像の説明 (嗜好・傾向)。 */
  descriptor?: string;
}

export interface FlowPersona {
  id: string;
  name: string;
  role: FlowRole;
  /** セッション固定の立場 (respec 01)。facilitator=neutral / opinion=opinion / debater=pro|con。 */
  stance: FlowStance;
  speechStyle: string;
  traits: string[];
  model: string;
  isLocal: boolean;
  /** このペルソナが重視する価値軸 (persona-setup が LLM で生成。失敗時は undefined で degrade)。 */
  valueAxis?: string;
  /** debater の核となる主張 1〜2 個 (persona-setup が LLM で生成)。 */
  coreClaims?: string[];
  /** 憑依しているデータ由来ペルソナ (B)。無ければ通常の生成ペルソナ。 */
  possession?: PersonaPossession;
}

// ── 人格プール (composition.ts と同じプールを流用) ──────────────────────────

const NAME_POOL = [
  "陽介", "凛", "ハル", "美咲", "翔", "葵", "蓮", "結衣", "拓海", "彩",
  "悠真", "詩織", "大樹", "舞", "颯太", "真央", "啓介", "杏奈", "亮", "千夏",
];

const SURNAME_POOL = [
  "佐伯", "三上", "高瀬", "橘", "藤堂", "水野", "神谷", "篠原", "久我", "朝倉",
  "一条", "白石", "桐生", "成瀬", "榊", "相馬", "黒川", "有馬", "二宮", "望月",
];

function toFullName(givenName: string, ordinal: number): string {
  const name = givenName.trim();
  if (!name) return `${SURNAME_POOL[ordinal % SURNAME_POOL.length]} 名称未設定`;
  if (name.includes(" ") || name.includes("　")) return name;
  return `${SURNAME_POOL[ordinal % SURNAME_POOL.length]} ${name}`;
}

const FULL_NAME_POOL = NAME_POOL.map((name, i) => toFullName(name, i));

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
  const free = FULL_NAME_POOL.filter((n) => !used.has(n));
  const pool = free.length > 0 ? free : FULL_NAME_POOL;
  const name = pool[Math.floor(rng() * pool.length)] ?? FULL_NAME_POOL[0];
  const final = used.has(name) ? `${name}${used.size}` : name;
  used.add(final);
  return final;
}

/** ロールから導出する既定スタンス (debater はセッション割当が正で、これはフォールバック)。 */
export function roleDefaultStance(role: FlowRole): FlowStance {
  if (role === "facilitator") return "neutral";
  if (role === "opinion") return "opinion";
  return "pro";
}

/**
 * ペルソナのセッション固定スタンスを返す (旧 decideStance の置換、純関数)。
 * facilitator → neutral / opinion → opinion は従来通り。
 * debater は生成時に固定した persona.stance (無ければロール既定にフォールバック)。
 */
export function personaStance(persona: FlowPersona): FlowStance {
  if (persona.role === "facilitator") return "neutral";
  if (persona.role === "opinion") return "opinion";
  return persona.stance === "pro" || persona.stance === "con"
    ? persona.stance
    : roleDefaultStance(persona.role);
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
 * debater の stance は pro/con に均等割 (奇数なら rng で余り 1 を振る) でセッション固定する。
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
    stance: "neutral",
    speechStyle: ROLE_FLAVOR.facilitator.style,
    traits: ROLE_FLAVOR.facilitator.traits,
    model: defaultModel,
    isLocal,
  });

  // 残りの人数を opinion と debater に 1:2 で配分
  const remaining = count - 1;
  const opinionCount = remaining > 0 ? Math.max(1, Math.floor(remaining / 3)) : 0;
  const debaterCount = remaining - opinionCount;

  for (let i = 0; i < opinionCount; i++) {
    personas.push({
      id: randomUUID(),
      name: pickName(used, rng),
      role: "opinion",
      stance: "opinion",
      speechStyle: ROLE_FLAVOR.opinion.style,
      traits: ROLE_FLAVOR.opinion.traits,
      model: defaultModel,
      isLocal,
    });
  }

  // debater は pro/con に均等割。奇数なら rng で余り 1 を pro/con のどちらかに振る。
  const proCount = Math.floor(debaterCount / 2) + (debaterCount % 2 === 1 && rng() < 0.5 ? 1 : 0);
  for (let i = 0; i < debaterCount; i++) {
    personas.push({
      id: randomUUID(),
      name: pickName(used, rng),
      role: "debater",
      stance: i < proCount ? "pro" : "con",
      speechStyle: ROLE_FLAVOR.debater.style,
      traits: ROLE_FLAVOR.debater.traits,
      model: defaultModel,
      isLocal,
    });
  }

  return personas;
}

/**
 * ペルソナリストからランダムに 1 人選ぶ。
 * @deprecated 議論フローのターンはシャッフル輪番 (turn-scheduler.buildTurnOrder) に移行済み。
 * 壁打ち (sparring) の不定期応答でのみ使う。
 */
export function pickRandomPersona(personas: FlowPersona[], rng: Rng): FlowPersona {
  const idx = Math.floor(rng() * personas.length);
  return personas[idx];
}
