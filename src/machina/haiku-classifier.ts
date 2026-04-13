/**
 * 「タスク性」判定器。
 *
 * 環境変数 ANTHROPIC_API_KEY がセットされていれば Claude Haiku を呼ぶ。
 * 未設定時は同梱のルールベース analyzer にフォールバックする。
 *
 * 返り値は以下のシンプルな構造:
 *   - isTask          : タスクと判断したか
 *   - confidence      : 0-1 の確度
 *   - missingFields   : 登録するにあたり不足している情報 (title/assignee/due など)
 *   - title/description/priority : 抽出できた値
 *   - reasoning       : 判断根拠 (1-2 文)
 */

import { analyzeMessage } from "./analyzer.js";

export interface HaikuTaskClassification {
  isTask: boolean;
  confidence: number;
  missingFields: string[];
  title?: string;
  description?: string;
  priority?: string;
  reasoning: string;
}

const HAIKU_MODEL = process.env.HAIKU_MODEL ?? "claude-haiku-4-5-20251001";
const API_KEY = process.env.ANTHROPIC_API_KEY;

/** メッセージとスレッドコンテキストを渡して分類 */
export async function classifyMessage(args: {
  text: string;
  threadContext?: Array<{ authorName: string; text: string }>;
  platform: "slack" | "discord";
}): Promise<HaikuTaskClassification> {
  if (API_KEY) {
    try {
      return await classifyWithHaiku(args);
    } catch (err) {
      console.warn("[haiku-classifier] Haiku 呼び出しに失敗、ルールベースにフォールバック:", err);
    }
  }
  return classifyWithRuleset(args);
}

// ─── LLM implementation ───────────────────────────────

async function classifyWithHaiku(args: {
  text: string;
  threadContext?: Array<{ authorName: string; text: string }>;
  platform: "slack" | "discord";
}): Promise<HaikuTaskClassification> {
  const contextText = (args.threadContext ?? [])
    .map((m) => `${m.authorName}: ${m.text}`)
    .join("\n");

  const systemPrompt = `あなたは ${args.platform} のチャット投稿を解析し、
「タスクとして管理すべき内容か」「情報が十分か」を判定するアシスタントです。
必ず JSON だけを返してください。`;

  const userPrompt = [
    contextText ? `# スレッドコンテキスト\n${contextText}\n` : "",
    `# 新しい投稿\n${args.text}`,
    "",
    "上記に基づき、以下の JSON スキーマで判定してください:",
    "{",
    '  "isTask": boolean,',
    '  "confidence": 0-1 の数値,',
    '  "title": 抽出したタスクタイトル (短め),',
    '  "description": 補足説明 (任意),',
    '  "priority": "low" | "medium" | "high" | "critical",',
    '  "missingFields": string[] // "title","assignee","due","context" など不足しているもの',
    '  "reasoning": 1-2 文の日本語の判断根拠',
    "}",
  ].join("\n");

  const res = await fetch("https://api.anthropic.com/v1/messages", {
    method: "POST",
    headers: {
      "x-api-key": API_KEY!,
      "anthropic-version": "2023-06-01",
      "content-type": "application/json",
    },
    body: JSON.stringify({
      model: HAIKU_MODEL,
      max_tokens: 512,
      system: systemPrompt,
      messages: [{ role: "user", content: userPrompt }],
    }),
  });

  if (!res.ok) {
    throw new Error(`Haiku HTTP ${res.status}`);
  }

  const data = (await res.json()) as {
    content?: Array<{ type: string; text?: string }>;
  };

  const text = data.content?.find((c) => c.type === "text")?.text ?? "";
  const json = extractJson(text);
  if (!json) throw new Error("Haiku 応答から JSON を取得できませんでした");

  const parsed = JSON.parse(json) as Partial<HaikuTaskClassification>;
  return {
    isTask: Boolean(parsed.isTask),
    confidence: clamp(Number(parsed.confidence ?? 0), 0, 1),
    missingFields: Array.isArray(parsed.missingFields) ? parsed.missingFields : [],
    title: parsed.title,
    description: parsed.description ?? undefined,
    priority: parsed.priority,
    reasoning: parsed.reasoning ?? "",
  };
}

function extractJson(text: string): string | null {
  const first = text.indexOf("{");
  const last = text.lastIndexOf("}");
  if (first < 0 || last < 0 || last < first) return null;
  return text.slice(first, last + 1);
}

function clamp(n: number, min: number, max: number): number {
  if (Number.isNaN(n)) return min;
  return Math.max(min, Math.min(max, n));
}

// ─── Fallback: ruleset ────────────────────────────────

function classifyWithRuleset(args: {
  text: string;
  platform: "slack" | "discord";
}): HaikuTaskClassification {
  const result = analyzeMessage({ text: args.text, platform: args.platform });

  const missing: string[] = [];
  if (!result.title || result.title.trim().length < 4) missing.push("title");
  if (!result.assigneeHint) missing.push("assignee");
  if (!result.dueDateHint) missing.push("due");

  return {
    isTask: result.shouldCreateTask,
    confidence: result.confidence,
    missingFields: result.shouldCreateTask ? missing : [],
    title: result.title,
    description: result.description ?? undefined,
    priority: result.priority,
    reasoning:
      result.reasoning ||
      (result.shouldCreateTask
        ? "ルールベース判定: タスクキーワードを検出しました。"
        : "ルールベース判定: タスクキーワードは検出されませんでした。"),
  };
}
