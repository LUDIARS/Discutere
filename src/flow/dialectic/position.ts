/**
 * 定立 [1] — issue ごとに debater (pro/con 各代表) の Position を生成する (respec 06 §2)。
 *
 * 生成はメインモデル・温度高め (dialectic.md §5)。01 の coreClaims を種にする。
 * 構造化 JSON (claim + grounds 2〜4 + values) + 露出用口語文を同時に返させる。
 * パース失敗は degrade: 応答全文を claim = 露出文とする単一根拠の Position で受理 + warn
 * (発話を捨てない)。LLM 呼び出しのコストログは呼び出し側 (driver) が withCostLog で行う。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { FlowPersona } from "../personas.js";
import { extractJsonObject } from "../effect-predict.js";
import type { Ground, IssueRecord } from "./store.js";
import { coerceInferenceKind, PREMISE_RULES_TEXT } from "./premise-rules.js";

export interface GeneratedPosition {
  claim: string;
  /** 主張の限定 (どの層・どの条件の話か)。返らなければ null。 */
  qualifier: string | null;
  grounds: Ground[];
  values: string[];
  /** 露出用の口語文 (Discord/Web に流す)。 */
  text: string;
  /** JSON が取れず degrade したか。 */
  degraded: boolean;
}

export interface GeneratePositionArgs {
  theme: string;
  issue: IssueRecord;
  persona: FlowPersona;
  stance: "pro" | "con";
  /** ペーパー base (system に固定 = プロンプトキャッシュ戦略の維持)。 */
  paperSystem: string;
  /** withCostLog 済み LLM (location="position")。 */
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}

/** Position 生成プロンプト (テスト用に export)。 */
export function buildPositionPrompt(args: {
  persona: FlowPersona;
  stance: "pro" | "con";
  issue: IssueRecord;
}): string {
  const { persona, stance, issue } = args;
  const stanceJa = stance === "pro" ? "賛成" : "反対";
  const seed =
    persona.coreClaims && persona.coreClaims.length > 0
      ? `あなたの核となる主張 (これを種に組み立てる): ${persona.coreClaims.join(" / ")}\n`
      : "";
  return (
    `あなたは議論ペルソナ「${persona.name}」。` +
    `特徴: ${persona.traits.join(" / ")} / 話し方: ${persona.speechStyle}\n` +
    (persona.valueAxis ? `あなたが重視する価値: ${persona.valueAxis}\n` : "") +
    seed +
    `\n# 論点 ${issue.ordinal}\n${issue.title}\n\n` +
    `${PREMISE_RULES_TEXT}\n\n` +
    `この論点についてあなたは【${stanceJa}】の立場 (${stance}) で定立 (Position) を張ります。\n` +
    `次の JSON 1 個だけを返してください (前後に説明やコードフェンスを付けない):\n` +
    `{"claim": "<主張 1 文>", "qualifier": "<主張の限定: どのプレイヤー層・どの条件の話か>", ` +
    `"grounds": [{"kind": "<deduction|induction|abduction>", "data": "<データ: 事実・事例・仕様>", ` +
    `"warrant": "<論拠: データから主張が言える理由>"}], ` +
    `"values": ["<価値前提 (例: 収益 > 体験)>"], "text": "<Discord に流す口語 1〜3 文>"}\n` +
    `grounds は 2〜4 個。data と warrant は混ぜずに分ける。` +
    `text は実在の人間の雑談のような自然な口語で書く。`
  );
}

/** 根拠 id の採番 (stance 接頭辞で issue 内一意・決定的)。 */
export function groundId(stance: "pro" | "con", index: number): string {
  return `${stance}-G${index + 1}`;
}

function coerceStringArray(v: unknown, max: number): string[] {
  if (!Array.isArray(v)) return [];
  return v
    .filter((s): s is string => typeof s === "string" && s.trim().length > 0)
    .map((s) => s.trim())
    .slice(0, max);
}

type ParsedGround = Pick<Ground, "text"> & Partial<Pick<Ground, "kind" | "warrant">>;

/**
 * grounds を正規化する。トゥールミン形式 ({kind, data, warrant}) と旧形式 (文字列) の両方を受ける
 * (旧形式は kind/warrant 未設定 = データだけの根拠として扱う)。
 */
function coerceGrounds(v: unknown, max: number): ParsedGround[] {
  if (!Array.isArray(v)) return [];
  const out: ParsedGround[] = [];
  for (const item of v) {
    if (out.length >= max) break;
    if (typeof item === "string") {
      if (item.trim()) out.push({ text: item.trim() });
      continue;
    }
    if (!item || typeof item !== "object") continue;
    const o = item as Record<string, unknown>;
    const raw = typeof o.data === "string" ? o.data : typeof o.text === "string" ? o.text : "";
    const data = raw.trim();
    if (!data) continue;
    const kind = coerceInferenceKind(o.kind);
    const warrant = typeof o.warrant === "string" && o.warrant.trim() ? o.warrant.trim() : undefined;
    out.push({ text: data, ...(kind ? { kind } : {}), ...(warrant ? { warrant } : {}) });
  }
  return out;
}

/**
 * Position を 1 つ生成する。LLM エラーは throw せず degrade した Position を返す
 * (claim = エラー説明ではなく coreClaims/issue から機械組み立て。議論を止めない)。
 */
export async function generatePosition(args: GeneratePositionArgs): Promise<GeneratedPosition> {
  const { persona, stance, issue, llm, model, warn = () => {} } = args;

  const result = await llm.invoke({
    system: args.paperSystem,
    prompt: buildPositionPrompt({ persona, stance, issue }),
    ...(model ? { model } : {}),
  });

  if (!result.ok) {
    // LLM 障害: coreClaims (respec 01) を機械的に Position 化して degrade (warn 明示)。
    warn(`Position 生成 LLM エラー (${persona.name}/${stance}): ${result.error} — coreClaims で degrade`);
    const claim =
      persona.coreClaims?.[0] ?? `${issue.title} に${stance === "pro" ? "賛成" : "反対"}の立場を取る`;
    return {
      claim,
      qualifier: null,
      grounds: [{ id: groundId(stance, 0), text: claim, state: "unchallenged" }],
      values: persona.valueAxis ? [persona.valueAxis] : [],
      text: claim,
      degraded: true,
    };
  }

  const obj = extractJsonObject(result.text);
  if (!obj || typeof obj.claim !== "string" || !obj.claim.trim()) {
    warn(`Position 生成 JSON パース失敗 (${persona.name}/${stance}) — 全文を claim として受理 (degrade)`);
    const text = result.text.trim();
    return {
      claim: text.slice(0, 200) || issue.title,
      qualifier: null,
      grounds: [{ id: groundId(stance, 0), text: text.slice(0, 200) || issue.title, state: "unchallenged" }],
      values: [],
      text: text || issue.title,
      degraded: true,
    };
  }

  const claim = obj.claim.trim();
  const parsedGrounds = coerceGrounds(obj.grounds, 4);
  const grounds: Ground[] = (parsedGrounds.length > 0 ? parsedGrounds : [{ text: claim }]).map((g, i) => ({
    id: groundId(stance, i),
    text: g.text,
    state: "unchallenged",
    ...(g.kind ? { kind: g.kind } : {}),
    ...(g.warrant ? { warrant: g.warrant } : {}),
  }));
  const qualifier = typeof obj.qualifier === "string" && obj.qualifier.trim() ? obj.qualifier.trim() : null;
  const text = typeof obj.text === "string" && obj.text.trim() ? obj.text.trim() : claim;

  return {
    claim,
    qualifier,
    grounds,
    values: coerceStringArray(obj.values, 4),
    text,
    degraded: false,
  };
}
