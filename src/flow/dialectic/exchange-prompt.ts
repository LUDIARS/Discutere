/**
 * 論拠の応酬 [2] の応答プロンプト + パース (dialectic.md §2.6)。
 *
 * 応答者は相手の論拠 1 件に対して、次のどちらかだけを返す:
 *   - rebut   : 反論。反論も論拠 1 件 (データ + 論拠 + 種類) で、突く要素 (データ / 論拠 / 限定) を宣言する
 *   - concede : 認める (反論しない)
 * パース失敗は question (論拠として数えない) で受理する (発話は捨てない)。
 * 状態の判断はしない (exchange.ts / driver が持つ)。
 */

import type { FlowPersona, FlowStance } from "../personas.js";
import { extractJsonObject } from "../effect-predict.js";
import { stanceLine } from "../discussion-paper.js";
import type { ArgumentNode } from "./exchange.js";
import type { IssueRecord, PositionRecord } from "./store.js";
import { renderPositionDigest } from "./turn-prompt.js";
import {
  coerceAttackPoint,
  coerceInferenceKind,
  INFERENCE_KIND_LABEL,
  PREMISE_RULES_TEXT,
  type AttackPoint,
  type InferenceKind,
} from "./premise-rules.js";

export interface ExchangePromptArgs {
  persona: FlowPersona;
  stance: FlowStance;
  issue: IssueRecord;
  positions: readonly PositionRecord[];
  personaNames: ReadonlyMap<string, string>;
  /** 定立の論拠から応答対象までの経路 (根が先頭・末尾が応答対象)。 */
  path: readonly ArgumentNode[];
  voicesBlock?: string;
}

function renderNode(n: ArgumentNode, personaNames: ReadonlyMap<string, string>, selfId: string): string {
  const who = personaNames.get(n.personaId) ?? n.personaId;
  const owner = n.personaId === selfId ? `${who} (あなた)` : who;
  const kind = n.kind ? ` (${INFERENCE_KIND_LABEL[n.kind]})` : "";
  const label = n.depth === 0 ? `論拠 [${n.groundId ?? n.id}]` : `反論 ${n.depth}`;
  const warrant = n.warrant ? `\n    論拠: ${n.warrant}` : "";
  return `- ${label} ${owner}${kind}\n    データ: ${n.data}${warrant}\n    発言: ${n.text}`;
}

/** 応答プロンプト (system はペーパー base 固定)。 */
export function buildExchangePrompt(args: ExchangePromptArgs): string {
  const { persona, stance, issue, positions, personaNames, path } = args;
  const target = path[path.length - 1];
  const positionBlock = positions
    .map((p) => {
      const name = personaNames.get(p.personaId) ?? p.personaId;
      return renderPositionDigest(p, p.personaId === persona.id ? `${name} (あなた)` : name);
    })
    .join("\n\n");
  const chain = path.map((n) => renderNode(n, personaNames, persona.id)).join("\n");
  const targetName = target ? (personaNames.get(target.personaId) ?? target.personaId) : "相手";

  return [
    `あなたは議論ペルソナ「${persona.name}」。`,
    `特徴: ${persona.traits.join(" / ")} / 話し方: ${persona.speechStyle}`,
    ...(persona.valueAxis ? [`あなたが重視する価値: ${persona.valueAxis}`] : []),
    stanceLine(stance),
    "",
    PREMISE_RULES_TEXT,
    "",
    `# 論点 ${issue.ordinal}`,
    issue.title,
    "",
    ...(args.voicesBlock ? [args.voicesBlock, ""] : []),
    `# 両陣営の主張と論拠`,
    positionBlock || "(Position なし)",
    "",
    `# この論拠の応酬 (上から順に。最後が応答対象)`,
    chain,
    "",
    `# 指示`,
    `${targetName}さんの最後の${target && target.depth > 0 ? "反論" : "論拠"}に答えてください。答え方は次のどちらか 1 つ:`,
    `- rebut: 反論する。反論も論拠 1 つだけで立てる (データ 1 つ + 論拠 1 つ)`,
    `- concede: 相手の言い分を認める (崩せないと思うなら認める)`,
    `次の JSON 1 個だけを返す (前後に説明やコードフェンスを付けない):`,
    `{"act": "<rebut | concede>", "attack": "<rebut のとき必須: data | warrant | qualifier (相手のデータ・論拠・限定のどれを突くか)>", ` +
      `"kind": "<rebut のとき: deduction | induction | abduction>", ` +
      `"data": "<rebut のとき必須: 反論のデータ (事実・事例・仕様) 1 つ>", ` +
      `"warrant": "<rebut のとき必須: 反論の論拠 (そのデータで相手のどこが崩れるか) 1 つ>", ` +
      `"text": "<Discord に流す口語 1〜3 文>"}`,
    `反論は相手の主張の限定の内側で突く。論拠を 2 つ以上並べない。`,
    `text は実在の人間の雑談のような自然な口語で書く (ラベル・箇条書き禁止)。`,
  ].join("\n");
}

export interface ParsedExchangeTurn {
  act: "rebut" | "concede" | "question";
  attack: AttackPoint | null;
  kind: InferenceKind | null;
  data: string | null;
  warrant: string | null;
  text: string;
  degraded: boolean;
}

function str(v: unknown): string | null {
  return typeof v === "string" && v.trim() ? v.trim() : null;
}

/** 応答をパースする。JSON 崩れは question (論拠として数えない) で全文受理 + warn。 */
export function parseExchangeResponse(raw: string, warn: (msg: string) => void = () => {}): ParsedExchangeTurn {
  const obj = extractJsonObject(raw);
  const text = obj ? str(obj.text) : null;
  if (!obj || !text) {
    warn(`応酬の応答 JSON パース失敗 — question として受理 (degrade): ${raw.trim().slice(0, 60)}`);
    return { act: "question", attack: null, kind: null, data: null, warrant: null, text: raw.trim(), degraded: true };
  }
  const rawAct = str(obj.act)?.toLowerCase();
  const act = rawAct === "concede" ? "concede" : "rebut";
  if (rawAct !== "concede" && rawAct !== "rebut") warn(`応酬の act "${rawAct ?? ""}" は不明 — rebut として扱う`);
  return {
    act,
    attack: coerceAttackPoint(obj.attack),
    kind: coerceInferenceKind(obj.kind),
    data: str(obj.data),
    warrant: str(obj.warrant),
    text,
    degraded: false,
  };
}
