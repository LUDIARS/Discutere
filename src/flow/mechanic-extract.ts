/**
 * メカニクス LLM 増補 — ディスカッションペーパーの「ゲームのメカニクス」を厚くする。
 *
 * 供給源の data/games/<slug>.md は 1 ゲーム ~10 件程度しか持たず、議題によってはさらに薄い。
 * ここでは集めた感想 (外部の声) + 既存メカニクスを LLM に渡し、**語られている遊びのメカニクス**を
 * 追加抽出して目標件数まで増やす。既存件数が目標以上なら LLM を呼ばない。
 *
 * LLM 失敗 / JSON 解析失敗は既存メカニクスのまま degrade する (議論を止めない)。
 * DB 非依存・LLM 注入 — 単体テスト可能。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import type { MechanicSummary } from "./investigate.js";

/** 抽出プロンプトに載せる感想サンプルの最大件数。 */
const VOICE_SAMPLE = 25;

export interface EnrichMechanicsArgs {
  theme: string;
  /** investigate (game md) 由来の既存メカニクス。 */
  existing: MechanicSummary[];
  /** 集めた感想 (出所付き)。抽出の材料にする。 */
  voices: Array<{ content: string; source?: string }>;
  llm: LLMClient;
  /** 目標件数 (既定 30)。既存がこれ以上なら LLM を呼ばない。 */
  target: number;
  model?: string;
  warn?: (msg: string) => void;
}

/** 文字列から最初の JSON 配列を取り出す (コードフェンス/前置き混入に耐える)。 */
export function extractJsonArray(raw: string): unknown[] | null {
  const start = raw.indexOf("[");
  if (start < 0) return null;
  // まず素直に全体パース (正常時はこれで通る)。
  const end = raw.lastIndexOf("]");
  if (end > start) {
    try {
      const parsed = JSON.parse(raw.slice(start, end + 1));
      if (Array.isArray(parsed)) return parsed;
    } catch {
      // 救済へ
    }
  }
  // 救済: 配列内の {...} を 1 個ずつブレースマッチで取り出し個別パースする
  // (文字列内ブレースは無視。 maxTokens 到達等で途中で切れた末尾オブジェクトは自然に落ちる)。
  const objs: unknown[] = [];
  let depth = 0;
  let objStart = -1;
  let inStr = false;
  let esc = false;
  for (let i = start; i < raw.length; i++) {
    const ch = raw[i];
    if (inStr) {
      if (esc) esc = false;
      else if (ch === "\\") esc = true;
      else if (ch === '"') inStr = false;
      continue;
    }
    if (ch === '"') inStr = true;
    else if (ch === "{") {
      if (depth === 0) objStart = i;
      depth++;
    } else if (ch === "}") {
      if (depth > 0) depth--;
      if (depth === 0 && objStart >= 0) {
        try {
          objs.push(JSON.parse(raw.slice(objStart, i + 1)));
        } catch {
          // 壊れたオブジェクトは捨てる
        }
        objStart = -1;
      }
    }
  }
  return objs.length > 0 ? objs : null;
}

function normalizeMechanic(raw: unknown): MechanicSummary | null {
  if (!raw || typeof raw !== "object") return null;
  const m = raw as Record<string, unknown>;
  const name = typeof m.name === "string" ? m.name.trim() : "";
  if (!name) return null;
  const affect = typeof m.intended_affect === "string" ? m.intended_affect.trim() : "";
  return {
    name,
    description: typeof m.description === "string" ? m.description.trim() : "",
    ...(affect ? { intended_affect: affect } : {}),
  };
}

function buildPrompt(args: EnrichMechanicsArgs, need: number): { system: string; prompt: string } {
  const existingText =
    args.existing.length > 0
      ? args.existing.map((m) => `- ${m.name}: ${m.description}`).join("\n")
      : "(なし)";
  const voiceText =
    args.voices.length > 0
      ? args.voices
          .slice(0, VOICE_SAMPLE)
          .map((v, i) => `${i + 1}. ${v.content.slice(0, 160)}`)
          .join("\n")
      : "(感想なし)";
  const system =
    "あなたはゲームデザイン分析者です。 議題のゲームについて、 プレイヤーの感想から **遊びのメカニクス**" +
    " (プレイヤーの行動とその設計意図) を洗い出します。 既出と重複しない具体的なメカニクスを、" +
    " 感想に根ざして抽出してください。 返答は JSON 配列のみ。";
  const prompt =
    `# 議題\n${args.theme}\n\n# 既出メカニクス (重複させない)\n${existingText}\n\n` +
    `# プレイヤーの感想 (抽出の根拠)\n${voiceText}\n\n` +
    `上記の感想から読み取れる **追加の** 遊びのメカニクスを最大 ${need} 件、 次の JSON 配列だけで返してください` +
    " (前後に説明やコードフェンスを付けない):\n" +
    '[{"name":"メカニクス名","description":"プレイヤーの行動と設計意図 (1〜2文)","intended_affect":"狙う感情 (任意)"}]';
  return { system, prompt };
}

/** name (lower) で重複除去しつつ既存 → 追加の順に結合し、target 件で切る。 */
function mergeMechanics(existing: MechanicSummary[], added: MechanicSummary[], target: number): MechanicSummary[] {
  const seen = new Set(existing.map((m) => m.name.trim().toLowerCase()));
  const out = [...existing];
  for (const m of added) {
    const key = m.name.trim().toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(m);
    if (out.length >= target) break;
  }
  return out.slice(0, target);
}

/**
 * メカニクスを目標件数まで LLM で増補する。
 * 既存 >= target なら LLM を呼ばずそのまま返す。失敗時は既存のまま (degrade)。
 */
export async function enrichMechanics(args: EnrichMechanicsArgs): Promise<MechanicSummary[]> {
  const { existing, target, llm, model, warn = () => {} } = args;
  const need = target - existing.length;
  if (need <= 0) return existing.slice(0, target);

  let res;
  try {
    const { system, prompt } = buildPrompt(args, need);
    // need 件の詳細メカニクスを途中で切らさないよう余裕を持たせる (~250 tokens/件 目安 + 下限)。
    const maxTokens = Math.min(8000, Math.max(2000, need * 280));
    res = await llm.invoke({ system, prompt, model, maxTokens });
  } catch (e) {
    warn(`メカニクス増補で例外: ${(e as Error).message}`);
    return existing;
  }
  if (!res.ok) {
    warn(`メカニクス増補に失敗: ${res.error}`);
    return existing;
  }
  const arr = extractJsonArray(res.text);
  if (!arr) {
    warn("メカニクス増補の JSON 解析に失敗");
    return existing;
  }
  const added = arr.map(normalizeMechanic).filter((m): m is MechanicSummary => m !== null);
  return mergeMechanics(existing, added, target);
}
