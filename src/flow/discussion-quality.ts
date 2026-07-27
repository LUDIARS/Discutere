/**
 * 議論完了後の AI 品質確認。
 *
 * 「情報が足りているか」と「議論内容に意味があるか」を 1 回の構造化 LLM 呼び出しで評価し、
 * flow_conclusion に永続する。議論そのものが完了した後の補助評価なので、LLM 障害時は
 * unavailable を明示して結論を失わない。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import { withCostLog } from "./cost-logger.js";
import { getFlowDb } from "./db/connection.js";

export interface QualityDimension {
  score: number;
  rationale: string;
  missing: string[];
}

export type DiscussionQualityAssessment =
  | {
      status: "scored";
      informationSufficiency: QualityDimension;
      meaningfulness: QualityDimension;
      overallScore: number;
      assessedAt: number;
    }
  | {
      status: "unavailable";
      error: string;
      assessedAt: number;
    };

export interface DiscussionQualityInput {
  sessionId: string;
  theme: string;
  paper: string;
  conclusion: string;
  utterances: Array<{ personaName: string; text: string }>;
  llm: LLMClient;
  model?: string;
}

function extractJsonObject(text: string): Record<string, unknown> | null {
  const start = text.indexOf("{");
  const end = text.lastIndexOf("}");
  if (start < 0 || end <= start) return null;
  try {
    const parsed = JSON.parse(text.slice(start, end + 1));
    return parsed && typeof parsed === "object" && !Array.isArray(parsed)
      ? parsed as Record<string, unknown>
      : null;
  } catch {
    return null;
  }
}

function dimension(value: unknown, label: string): QualityDimension {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new TypeError(`${label} must be an object`);
  }
  const item = value as Record<string, unknown>;
  if (typeof item.score !== "number" || !Number.isFinite(item.score)) {
    throw new TypeError(`${label}.score must be a number`);
  }
  if (typeof item.rationale !== "string" || !item.rationale.trim()) {
    throw new TypeError(`${label}.rationale must be a non-empty string`);
  }
  const missing = Array.isArray(item.missing)
    ? item.missing.filter((entry): entry is string => typeof entry === "string" && entry.trim() !== "")
    : [];
  return {
    score: Math.max(0, Math.min(100, Math.round(item.score))),
    rationale: item.rationale.trim(),
    missing: [...new Set(missing.map((entry) => entry.trim()))].slice(0, 8),
  };
}

function promptFor(input: DiscussionQualityInput): string {
  const transcript = input.utterances
    .slice(-40)
    .map((utterance) => `${utterance.personaName}: ${utterance.text}`)
    .join("\n");
  return [
    "あなたは議論品質の監査役です。結論への賛否ではなく、入力と議論過程の品質を採点してください。",
    "informationSufficiency は仕様・施策・根拠・制約が結論を支えるだけ揃っているか、",
    "meaningfulness は論点の反復だけでなく比較・反証・トレードオフ・実行可能な示唆があるかを評価します。",
    "各 score は 0..100。根拠は具体的にし、不足が無ければ missing は空配列にしてください。",
    'JSONのみ: {"informationSufficiency":{"score":number,"rationale":string,"missing":string[]},"meaningfulness":{"score":number,"rationale":string,"missing":string[]}}',
    `# テーマ\n${input.theme}`,
    `# 議論仕様書・施策\n${input.paper || "(なし)"}`,
    `# 議論ログ\n${transcript || "(なし)"}`,
    `# 結論\n${input.conclusion || "(なし)"}`,
  ].join("\n\n");
}

export function saveDiscussionQuality(
  sessionId: string,
  assessment: DiscussionQualityAssessment
): void {
  const result = getFlowDb()
    .prepare(`UPDATE flow_conclusion SET quality_json = ? WHERE session_id = ?`)
    .run(JSON.stringify(assessment), sessionId);
  if (result.changes === 0) {
    throw new Error(`flow conclusion not found for quality assessment: ${sessionId}`);
  }
}

export function getDiscussionQuality(sessionId: string): DiscussionQualityAssessment | null {
  const row = getFlowDb()
    .prepare(`SELECT quality_json FROM flow_conclusion WHERE session_id = ?`)
    .get(sessionId) as { quality_json: string | null } | undefined;
  if (!row?.quality_json) return null;
  try {
    return JSON.parse(row.quality_json) as DiscussionQualityAssessment;
  } catch {
    return null;
  }
}

export async function assessAndSaveDiscussionQuality(
  input: DiscussionQualityInput
): Promise<DiscussionQualityAssessment> {
  const assessedAt = Date.now();
  const logged = withCostLog(input.llm, {
    flow: "discussion-quality",
    sessionId: input.sessionId,
    location: "quality-assessment",
  });
  const response = await logged.invoke({
    prompt: promptFor(input),
    model: input.model || undefined,
  });
  let assessment: DiscussionQualityAssessment;
  if (!response.ok) {
    assessment = { status: "unavailable", error: response.error, assessedAt };
  } else {
    try {
      const parsed = extractJsonObject(response.text);
      if (!parsed) throw new TypeError("quality response is not JSON");
      const informationSufficiency = dimension(parsed.informationSufficiency, "informationSufficiency");
      const meaningfulness = dimension(parsed.meaningfulness, "meaningfulness");
      assessment = {
        status: "scored",
        informationSufficiency,
        meaningfulness,
        overallScore: Math.round((informationSufficiency.score + meaningfulness.score) / 2),
        assessedAt,
      };
    } catch (error) {
      assessment = {
        status: "unavailable",
        error: `invalid AI assessment: ${(error as Error).message}`,
        assessedAt,
      };
    }
  }
  saveDiscussionQuality(input.sessionId, assessment);
  return assessment;
}
