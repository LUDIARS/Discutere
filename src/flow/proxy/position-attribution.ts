/**
 * 代理の定立 (Position) の論拠ごとの帰属検査 (S3)。
 *
 * 定立は議論の土台なので捨てずに受理し、本人の発言と申告した論拠が検査に通らなければ
 * 「推測」に落とす (本人の発言としては扱わない)。露出文には帰属の印を付ける。
 * 代理でなければ入力をそのまま返す (通常議論は無改変)。
 */

import type { FlowPersona } from "../personas.js";
import type { GeneratedPosition } from "../dialectic/position.js";
import { attributionSuffix, checkAttribution } from "./attribution.js";

export function attributePositionGrounds(
  gp: GeneratedPosition,
  persona: FlowPersona,
  warn: (msg: string) => void = () => {}
): GeneratedPosition {
  const scope = persona.proxy;
  if (!scope) return gp;
  const grounds = gp.grounds.map((g) => {
    const result = checkAttribution({
      claimKind: g.claimKind ?? null,
      evidenceIds: g.evidenceIds ?? [],
      data: g.text,
      scope,
    });
    if (!result.ok) {
      warn(`代理 ${persona.name} の論拠 ${g.id}: 帰属の検査に通らない (${result.foul}) — 推測として扱う`);
      return { ...g, claimKind: "simulationInference" as const, evidenceIds: result.evidenceIds };
    }
    return { ...g, claimKind: result.claimKind, evidenceIds: result.evidenceIds };
  });
  const groundTexts = gp.groundTexts.map((text, i) => {
    const g = grounds[i];
    return g ? `${text}${attributionSuffix(g.claimKind, g.evidenceIds)}` : text;
  });
  return { ...gp, grounds, groundTexts };
}
