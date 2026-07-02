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

export interface GeneratedPosition {
  claim: string;
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
    `この論点についてあなたは【${stanceJa}】の立場 (${stance}) で定立 (Position) を張ります。\n` +
    `次の JSON 1 個だけを返してください (前後に説明やコードフェンスを付けない):\n` +
    `{"claim": "<主張 1 文>", "grounds": ["<根拠 1>", "<根拠 2>"], ` +
    `"values": ["<価値前提 (例: 収益 > 体験)>"], "text": "<Discord に流す口語 1〜3 文>"}\n` +
    `grounds は 2〜4 個。text は実在の人間の雑談のような自然な口語で書く。`
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
      grounds: [{ id: groundId(stance, 0), text: text.slice(0, 200) || issue.title, state: "unchallenged" }],
      values: [],
      text: text || issue.title,
      degraded: true,
    };
  }

  const groundTexts = coerceStringArray(obj.grounds, 4);
  const claim = obj.claim.trim();
  const grounds: Ground[] = (groundTexts.length > 0 ? groundTexts : [claim]).map((text, i) => ({
    id: groundId(stance, i),
    text,
    state: "unchallenged",
  }));
  const text = typeof obj.text === "string" && obj.text.trim() ? obj.text.trim() : claim;

  return {
    claim,
    grounds,
    values: coerceStringArray(obj.values, 4),
    text,
    degraded: false,
  };
}
