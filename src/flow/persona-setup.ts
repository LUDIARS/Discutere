/**
 * ペルソナのセッション初期化 (respec 01, A9)。
 *
 * 「N 人の議論」が 1 モデル × 3 プロンプト風味にならないよう、セッション開始時に
 * **1 回だけ** LLM を呼び、全ペルソナぶんの「価値軸 + 核主張」をまとめて生成する。
 * 生成結果はキャストごと flow_session_persona に永続する (議論の再現性・レビュー可能性。
 * PR-B の信念状態はこの表に belief_json を足して育てる前提の置き場所)。
 *
 * LLM 失敗・JSON 不正は valueAxis なしで degrade (warn。議論は止めない)。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import type { FlowPersona } from "./personas.js";
import { withCostLog } from "./cost-logger.js";
import { getFlowDb } from "./db/connection.js";

/** 価値軸/核主張生成の 1 ペルソナぶんの LLM 出力。 */
interface PersonaValueAssignment {
  personaId: string;
  valueAxis?: string;
  coreClaims?: string[];
}

export interface SetupSessionPersonasArgs {
  theme: string;
  sessionId: string;
  personas: FlowPersona[];
  llm: LLMClient;
  /** フロー種別 (コストログ用)。既定 "discussion"。 */
  flow?: string;
  /** 生成に使うモデル (config flow.personaSetupModel。"" / 未指定ならメインモデル)。 */
  model?: string;
  warn?: (msg: string) => void;
}

/** 価値軸/核主張の一括生成プロンプトを組み立てる (テスト用に export)。 */
export function buildPersonaSetupPrompt(theme: string, personas: readonly FlowPersona[]): string {
  const cast = personas
    .map((p) => `- id: ${p.id} / 名前: ${p.name} / ロール: ${p.role} / スタンス: ${p.stance}`)
    .join("\n");
  return (
    `テーマ「${theme}」の議論に参加するペルソナ一覧です。\n` +
    `各ペルソナに「重視する価値 (価値軸)」を 1 文で割り当ててください。\n` +
    `debater には加えて「核となる主張」を 1〜2 個割り当ててください\n` +
    `(pro と con は対立が成立するよう主張を対にする。facilitator / opinion は coreClaims 省略可)。\n\n` +
    `# ペルソナ\n${cast}\n\n` +
    `返答は次の JSON 配列のみ (説明不要):\n` +
    `[{ "personaId": "<id>", "valueAxis": "<重視する価値 1 文>", "coreClaims": ["<核主張>"] }]`
  );
}

/** LLM 応答テキストから最初の JSON 配列を取り出す (壊れていれば throw)。 */
function extractJsonArray(text: string): unknown[] {
  const start = text.indexOf("[");
  const end = text.lastIndexOf("]");
  if (start < 0 || end <= start) throw new Error("no JSON array in LLM output");
  const parsed = JSON.parse(text.slice(start, end + 1));
  if (!Array.isArray(parsed)) throw new Error("LLM output is not a JSON array");
  return parsed;
}

/** LLM 出力を PersonaValueAssignment[] に検証しつつ変換する (不正要素は捨てる)。 */
function coerceAssignments(raw: unknown[]): PersonaValueAssignment[] {
  const out: PersonaValueAssignment[] = [];
  for (const item of raw) {
    if (!item || typeof item !== "object") continue;
    const o = item as Record<string, unknown>;
    if (typeof o.personaId !== "string" || !o.personaId) continue;
    const valueAxis = typeof o.valueAxis === "string" && o.valueAxis.trim() ? o.valueAxis.trim() : undefined;
    const coreClaims = Array.isArray(o.coreClaims)
      ? o.coreClaims.filter((c): c is string => typeof c === "string" && c.trim().length > 0).slice(0, 2)
      : undefined;
    out.push({ personaId: o.personaId, valueAxis, coreClaims });
  }
  return out;
}

/** キャスト全員を flow_session_persona に永続する (価値軸生成の成否に依らず呼ぶ)。 */
export function persistSessionPersonas(sessionId: string, personas: readonly FlowPersona[]): void {
  const db = getFlowDb();
  const now = Date.now();
  const stmt = db.prepare(
    `INSERT OR REPLACE INTO flow_session_persona
       (id, session_id, name, role, stance, value_axis, core_claims_json, possession_name, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`
  );
  for (const p of personas) {
    stmt.run(
      p.id,
      sessionId,
      p.name,
      p.role,
      p.stance,
      p.valueAxis ?? null,
      p.coreClaims ? JSON.stringify(p.coreClaims) : null,
      p.possession?.label ?? null,
      now
    );
  }
}

/**
 * セッションのキャストに価値軸/核主張を割り当て (LLM 1 call)、flow_session_persona に永続する。
 * 返り値は valueAxis / coreClaims をマージした新しいペルソナ列 (入力は変更しない)。
 * LLM 失敗時は warn を出し、割り当てなしのまま永続して続行する (degrade 明示)。
 */
export async function setupSessionPersonas(args: SetupSessionPersonasArgs): Promise<FlowPersona[]> {
  const { theme, sessionId, personas, llm, flow = "discussion", model, warn = console.warn } = args;

  const logged = withCostLog(llm, {
    flow,
    sessionId,
    location: "persona-setup",
  });

  let enriched = personas;
  const result = await logged.invoke({
    prompt: buildPersonaSetupPrompt(theme, personas),
    ...(model ? { model } : {}),
  });

  if (!result.ok) {
    warn(`ペルソナ価値軸生成 失敗: ${result.error} — 価値軸なしで議論を続行 (degrade)`);
  } else {
    try {
      const byId = new Map(coerceAssignments(extractJsonArray(result.text)).map((a) => [a.personaId, a]));
      enriched = personas.map((p) => {
        const a = byId.get(p.id);
        if (!a) return p;
        return {
          ...p,
          ...(a.valueAxis ? { valueAxis: a.valueAxis } : {}),
          // coreClaims は debater のみ採用 (spec 01: 他ロールは注入しない)。
          ...(p.role === "debater" && a.coreClaims?.length ? { coreClaims: a.coreClaims } : {}),
        };
      });
    } catch (e) {
      warn(
        `ペルソナ価値軸生成 JSON パース失敗: ${(e as Error).message} — 価値軸なしで議論を続行 (degrade)`
      );
    }
  }

  persistSessionPersonas(sessionId, enriched);
  return enriched;
}
