/**
 * 前提ルール違反の検査 (dialectic.md §2.5) — 反論 (rebut) を論証グラフへ入れる前のゲート。
 *
 * 2 段で検査する:
 *   1. checkRebutAnchor (コードのみ・決定的): Position への反論は、突くトゥールミン要素
 *      (データ / 論拠 / 限定) を宣言し、データ・論拠なら攻撃対象の根拠 id を指していなければならない。
 *   2. judgeRuleViolation (判定 LLM・小モデル): 宣言した要素を本当に突いているか、
 *      前提の外から主張を曲げていないか (範囲外・すり替え・中身に触れない却下・論点ずらし) を単一ラベルで判定。
 *      生成と判定は混ぜない (dialectic.md §5-1)。LLM 障害・ラベル不明は ok に倒す (議論を止めない、warn 明示)。
 *
 * 違反した反論は発話としては残すが、根拠を challenged にしない (driver が question に格下げする)。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import {
  ATTACK_POINT_LABEL,
  INFERENCE_KIND_LABEL,
  type AttackPoint,
} from "./premise-rules.js";
import type { Ground, IssueRecord, PositionRecord } from "./store.js";

/** 違反の種類。unanchored だけがコード判定、他は判定 LLM。 */
export type RuleFoul =
  | "unanchored"
  | "out_of_scope"
  | "straw_man"
  | "dismissal"
  | "topic_shift";

const LLM_FOULS: readonly Exclude<RuleFoul, "unanchored">[] = [
  "out_of_scope",
  "straw_man",
  "dismissal",
  "topic_shift",
];

export const RULE_FOUL_LABEL: Record<RuleFoul, string> = {
  unanchored: "どの根拠のデータ・論拠・限定を突くのかが示されていない",
  out_of_scope: "相手の主張の限定の外側から全体を否定している",
  straw_man: "相手が言っていない主張に置き換えている",
  dismissal: "根拠の中身に触れずに退けている",
  topic_shift: "論点をずらしている",
};

export type AnchorResult =
  | { ok: true; ground: Ground | null }
  | { ok: false; foul: "unanchored" };

/**
 * 反論のアンカー検査 (決定的)。
 * - attack 未宣言 → 違反
 * - data / warrant → groundId が反論先 Position の未譲歩の根拠を指していなければ違反
 * - qualifier → 主張の限定そのものを突くので根拠 id は不要 (ground=null)
 */
export function checkRebutAnchor(args: {
  position: PositionRecord;
  attack: AttackPoint | null;
  groundId: string | null;
}): AnchorResult {
  const { position, attack, groundId } = args;
  if (!attack) return { ok: false, foul: "unanchored" };
  if (attack === "qualifier") return { ok: true, ground: null };
  const ground = groundId
    ? position.grounds.find((g) => g.id === groundId && g.state !== "conceded")
    : undefined;
  return ground ? { ok: true, ground } : { ok: false, foul: "unanchored" };
}

function renderTarget(position: PositionRecord, ground: Ground | null): string {
  const lines = [
    `主張: ${position.claim}`,
    `限定: ${position.qualifier ?? "(明示なし)"}`,
  ];
  if (ground) {
    lines.push(`突かれた根拠 [${ground.id}]` + (ground.kind ? ` (${INFERENCE_KIND_LABEL[ground.kind]})` : ""));
    lines.push(`  データ: ${ground.text}`);
    lines.push(`  論拠: ${ground.warrant ?? "(明示なし)"}`);
  }
  return lines.join("\n");
}

/** 判定プロンプト (テスト用に export)。 */
export function buildRuleJudgePrompt(args: {
  issue: IssueRecord;
  position: PositionRecord;
  ground: Ground | null;
  attack: AttackPoint;
  rebutText: string;
}): string {
  return (
    `# 論点\n${args.issue.title}\n\n` +
    `# 反論された側\n${renderTarget(args.position, args.ground)}\n\n` +
    `# 反論 (「${ATTACK_POINT_LABEL[args.attack]}」を突くと宣言)\n${args.rebutText}\n\n` +
    `この反論が議論の前提ルールを守っているかを判定し、ラベルを 1 つだけ返してください (説明不要):\n` +
    `- ok: 宣言した要素 (データの真偽・論拠の妥当性・限定の範囲) を、相手の主張の限定の内側で突いている\n` +
    `- out_of_scope: 相手の限定の外側にある層・条件・事例を持ち出して主張全体を否定している\n` +
    `- straw_man: 相手が言っていない主張に置き換えて否定している\n` +
    `- dismissal: 根拠の中身に触れず「感想にすぎない」「データが無い」だけで退けている (何が足りないかを具体的に示していない)\n` +
    `- topic_shift: この論点と別の論点に話をずらしている\n` +
    `判断に迷う場合は ok を返す。`
  );
}

/** 応答テキストからラベルを取り出す (最初に現れた既知ラベル。ok も含む)。 */
export function parseRuleVerdict(text: string): RuleFoul | "ok" | null {
  const lower = text.toLowerCase();
  let best: { label: RuleFoul | "ok"; index: number } | null = null;
  for (const label of [...LLM_FOULS, "ok"] as const) {
    const idx = lower.indexOf(label);
    if (idx >= 0 && (best === null || idx < best.index)) best = { label, index: idx };
  }
  return best?.label ?? null;
}

export interface JudgeRuleArgs {
  issue: IssueRecord;
  position: PositionRecord;
  ground: Ground | null;
  attack: AttackPoint;
  rebutText: string;
  /** withCostLog 済み判定 LLM (judgeModel, location="rule-check")。 */
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}

/** 判定 LLM で前提ルール違反を検査する。障害・ラベル不明は ok (degrade)。 */
export async function judgeRuleViolation(args: JudgeRuleArgs): Promise<RuleFoul | "ok"> {
  const { llm, model, warn = () => {} } = args;
  const result = await llm.invoke({
    prompt: buildRuleJudgePrompt(args),
    ...(model ? { model } : {}),
    maxTokens: 50,
  });
  if (!result.ok) {
    warn(`前提ルール判定 LLM エラー: ${result.error} — ok として続行 (degrade)`);
    return "ok";
  }
  const verdict = parseRuleVerdict(result.text);
  if (!verdict) {
    warn(`前提ルール判定ラベル不明: "${result.text.slice(0, 60)}" — ok として続行 (degrade)`);
    return "ok";
  }
  return verdict;
}

export type RebutGateResult =
  | { accepted: true; ground: Ground | null }
  | { accepted: false; foul: RuleFoul };

/**
 * Position への反論 1 件をゲートする (アンカー検査 → 判定 LLM)。
 * judge=false なら判定 LLM を呼ばずアンカー検査だけで通す (config flow.dialectic.ruleCheck)。
 */
export async function gateRebut(args: {
  issue: IssueRecord;
  position: PositionRecord;
  attack: AttackPoint | null;
  groundId: string | null;
  rebutText: string;
  judge: boolean;
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}): Promise<RebutGateResult> {
  const anchor = checkRebutAnchor(args);
  if (!anchor.ok) return { accepted: false, foul: anchor.foul };
  if (!args.judge || !args.attack) return { accepted: true, ground: anchor.ground };
  const verdict = await judgeRuleViolation({
    issue: args.issue,
    position: args.position,
    ground: anchor.ground,
    attack: args.attack,
    rebutText: args.rebutText,
    llm: args.llm,
    model: args.model,
    warn: args.warn,
  });
  return verdict === "ok" ? { accepted: true, ground: anchor.ground } : { accepted: false, foul: verdict };
}

/** 違反時に進行役が流す一言 (露出用)。 */
export function renderFoulNotice(personaName: string, foul: RuleFoul): string {
  return (
    `${personaName}さんの今の反論は「${RULE_FOUL_LABEL[foul]}」ので、根拠への反論としては数えずに問いとして扱いますね。` +
    `相手の主張の範囲の中で、データ・論拠・限定のどれが違うのかを示してもらえると議論が進みます。`
  );
}
