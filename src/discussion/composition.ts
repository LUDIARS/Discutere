/**
 * 議論パーティ編成 (pure)。
 *
 * - 総数 = max(minTotal, ceil(想定発話数 / 4) + 1)
 * - 内訳: 司会 ×1 + キーマン ×keymanCount + 意見 ×(残り)
 * - 司会 / キーマン = 固定モデル (既定 Opus)、意見 = weighted 抽選 (既定 Sonnet:Haiku=6:4)
 * - 賛否 (pro/con) はペルソナに固定せず、ターン時に decideStance() で振る
 * - 続行時の半数入替は swapHalf(): 司会は不変、キーマンはモデル維持で別人格、
 *   意見はモデルも再抽選
 *
 * RNG は注入式 ([0,1) を返す)。テストは決定論的 rng で検証する。
 */

export type Tier = "facilitator" | "keyman" | "opinion";
export type Stance = "pro" | "con" | "neutral";
export type Rng = () => number;

export interface WeightedModel {
  model: string;
  /** 相対重み (正の数)。例 Sonnet=6 / Haiku=4 */
  weight: number;
}

export interface PartyConfig {
  /** 想定発話数。総数算出と続行ゲートの基準。 */
  expectedUtterances: number;
  /** キーマン人数 (既定 2)。 */
  keymanCount: number;
  /** 司会のモデル (既定 Opus)。 */
  facilitatorModel: string;
  /** キーマンのモデル (既定 Opus、入替後も維持)。 */
  keymanModel: string;
  /** 意見メンバーのモデル抽選表 (既定 [{sonnet,6},{haiku,4}])。 */
  opinionModels: WeightedModel[];
  /** 総数の下限 (既定 4)。 */
  minTotal: number;
}

export interface PartyMember {
  /** スロット id (パーティ内で安定、入替で人格は変わるが slot は残る)。例 "keyman-1" */
  slotId: string;
  tier: Tier;
  /** CLI に渡すモデル id。 */
  model: string;
  /** 表示名 (人間名)。 */
  personaName: string;
  /** 口調。 */
  speechStyle: string;
  /** 性格特徴。 */
  traits: string[];
}

/** 総数を算出する。 */
export function partySize(expectedUtterances: number, minTotal = 4): number {
  return Math.max(minTotal, Math.ceil(expectedUtterances / 4) + 1);
}

/** weighted 抽選でモデルを 1 つ選ぶ。 */
export function pickWeightedModel(models: WeightedModel[], rng: Rng): string {
  const total = models.reduce((s, m) => s + Math.max(0, m.weight), 0);
  if (total <= 0) return models[0]?.model ?? "";
  let r = rng() * total;
  for (const m of models) {
    r -= Math.max(0, m.weight);
    if (r < 0) return m.model;
  }
  return models[models.length - 1].model;
}

// ── 人格 (名前 / 口調 / 特徴) プール ──
const NAME_POOL = [
  "陽介", "凛", "ハル", "美咲", "翔", "葵", "蓮", "結衣", "拓海", "彩",
  "悠真", "詩織", "大樹", "舞", "颯太", "真央", "啓介", "杏奈", "亮", "千夏",
];

const TIER_FLAVOR: Record<Tier, { style: string; traits: string[] }> = {
  facilitator: { style: "穏やかな進行役口調", traits: ["中立", "俯瞰", "巻き込み上手"] },
  keyman: { style: "芯のある説得口調", traits: ["論点を掴む", "具体例豊富", "粘り強い"] },
  opinion: { style: "自然な雑談口調", traits: ["独自の角度", "素直", "発想が柔らかい"] },
};

/** 未使用の名前を 1 つ引く (尽きたら index サフィックスで一意化)。 */
function pickName(used: Set<string>, rng: Rng): string {
  const free = NAME_POOL.filter((n) => !used.has(n));
  const pool = free.length > 0 ? free : NAME_POOL;
  const name = pool[Math.floor(rng() * pool.length)] ?? NAME_POOL[0];
  const final = used.has(name) ? `${name}${used.size}` : name;
  used.add(final);
  return final;
}

function makeMember(slotId: string, tier: Tier, model: string, used: Set<string>, rng: Rng): PartyMember {
  const flavor = TIER_FLAVOR[tier];
  return { slotId, tier, model, personaName: pickName(used, rng), speechStyle: flavor.style, traits: flavor.traits };
}

/** パーティを編成する。 */
export function formParty(cfg: PartyConfig, rng: Rng): PartyMember[] {
  const total = partySize(cfg.expectedUtterances, cfg.minTotal);
  const opinionCount = Math.max(1, total - 1 - cfg.keymanCount);
  const used = new Set<string>();
  const members: PartyMember[] = [];

  members.push(makeMember("facilitator", "facilitator", cfg.facilitatorModel, used, rng));
  for (let i = 1; i <= cfg.keymanCount; i++) {
    members.push(makeMember(`keyman-${i}`, "keyman", cfg.keymanModel, used, rng));
  }
  for (let i = 1; i <= opinionCount; i++) {
    members.push(makeMember(`opinion-${i}`, "opinion", pickWeightedModel(cfg.opinionModels, rng), used, rng));
  }
  return members;
}

/**
 * 続行時の半数入替。
 * - 司会は不変。
 * - 入替対象 = キーマン + 意見。そのうち floor(対象数 / 2) 名をランダムに選び再生成。
 * - キーマンはモデル維持 (Opus) で別人格、意見はモデルも再抽選。
 */
export function swapHalf(party: PartyMember[], cfg: PartyConfig, rng: Rng): PartyMember[] {
  const swappable = party.filter((m) => m.tier !== "facilitator");
  const swapCount = Math.floor(swappable.length / 2);
  // 入替対象スロットをランダムに swapCount 個選ぶ (Fisher-Yates 部分シャッフル)。
  const idx = swappable.map((_, i) => i);
  for (let i = idx.length - 1; i > 0; i--) {
    const j = Math.floor(rng() * (i + 1));
    [idx[i], idx[j]] = [idx[j], idx[i]];
  }
  const swapSlots = new Set(idx.slice(0, swapCount).map((i) => swappable[i].slotId));

  const used = new Set(party.filter((m) => !swapSlots.has(m.slotId)).map((m) => m.personaName));
  return party.map((m) => {
    if (!swapSlots.has(m.slotId)) return m;
    const model = m.tier === "keyman" ? cfg.keymanModel : pickWeightedModel(cfg.opinionModels, rng);
    return makeMember(m.slotId, m.tier, model, used, rng);
  });
}

/** ターン時の賛否決定。司会は neutral、キーマン/意見は pro/con を 50:50。 */
export function decideStance(member: PartyMember, rng: Rng): Stance {
  if (member.tier === "facilitator") return "neutral";
  return rng() < 0.5 ? "pro" : "con";
}
