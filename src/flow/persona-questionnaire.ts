import { randomUUID } from "node:crypto";
import type { LLMClient } from "../persona-engine/llm/client.js";
import type { FlowRole } from "./personas.js";
import { extractJsonArray } from "./mechanic-extract.js";
import { insertPoolPersona, type PoolPersona } from "./persona-pool.js";
import { averageVectors } from "./persona-synthesize.js";
import { DIM, VECTOR_DIMS, subtract, textToVector } from "./sentiment-vector.js";

export type PersonaQuestionKind = "preference_metric" | "game_usage" | "game_specific";

export interface PersonaQuestion {
  id: string;
  kind: PersonaQuestionKind;
  metric: string;
  question: string;
  options?: string[];
  vectorHints: string[];
  weight: number;
}

export interface PersonaQuestionnaire {
  gameTitle: string;
  gameSlug?: string;
  mechanicsContext?: string;
  baselineText: string;
  baselineVector: number[];
  questions: PersonaQuestion[];
  source: "llm" | "fallback";
}

export interface BuildPersonaQuestionnaireArgs {
  gameTitle: string;
  gameSlug?: string;
  mechanicsContext?: string;
  userVoices?: string[];
  questionCount?: number;
  llm?: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}

export interface PersonaQuestionnaireAnswer {
  questionId: string;
  answer: unknown;
}

export type PersonaAnswersInput = Record<string, unknown> | readonly PersonaQuestionnaireAnswer[];

export interface PersonaVectorDelta {
  dim: string;
  delta: number;
}

export interface PersonaAnswerAnalysis {
  baselineVector: number[];
  affectVector: number[];
  responseVector: number[];
  topPositiveDeltas: PersonaVectorDelta[];
  topNegativeDeltas: PersonaVectorDelta[];
  answerVectors: Array<{ questionId: string; weight: number; vector: number[]; text: string }>;
}

export interface CreatePersonaFromAnswersArgs {
  questionnaire: PersonaQuestionnaire;
  answers: PersonaAnswersInput;
  name?: string;
  role?: FlowRole;
  llm?: LLMClient;
  model?: string;
  persist?: boolean;
  warn?: (msg: string) => void;
}

export interface CreatePersonaFromAnswersResult {
  persona: PoolPersona;
  analysis: PersonaAnswerAnalysis;
  saved: boolean;
}

interface PersonaMetadata {
  name: string;
  speechStyle: string;
  traits: string[];
  label: string;
}

const KIND_VALUES: readonly PersonaQuestionKind[] = ["preference_metric", "game_usage", "game_specific"];

function clampCount(value: number | undefined, fallback: number): number {
  if (value == null || !Number.isFinite(value)) return fallback;
  return Math.max(3, Math.min(24, Math.floor(value)));
}

function clampWeight(value: unknown): number {
  const n = typeof value === "number" ? value : Number(value);
  if (!Number.isFinite(n)) return 1;
  return Math.max(0.1, Math.min(3, +n.toFixed(2)));
}

function asStringArray(value: unknown): string[] {
  if (!Array.isArray(value)) return [];
  return value.map((v) => (typeof v === "string" ? v.trim() : "")).filter(Boolean).slice(0, 8);
}

function safeQuestionId(raw: unknown, fallback: string): string {
  const text = typeof raw === "string" ? raw.trim().toLowerCase() : "";
  const id = text.replace(/[^a-z0-9_-]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 40);
  return id || fallback;
}

function buildBaselineText(args: BuildPersonaQuestionnaireArgs): string {
  const parts = [
    `ゲーム: ${args.gameTitle}`,
    args.mechanicsContext ? `基本情報/メカニクス:\n${args.mechanicsContext}` : "",
    args.userVoices?.length ? `ユーザーの声:\n${args.userVoices.slice(0, 30).join("\n")}` : "",
  ].filter(Boolean);
  return parts.join("\n\n");
}

function baselineVector(args: BuildPersonaQuestionnaireArgs, baselineText: string): number[] {
  const vectors = [textToVector(baselineText || args.gameTitle)];
  for (const voice of args.userVoices ?? []) {
    const text = voice.trim();
    if (text) vectors.push(textToVector(text));
  }
  return averageVectors(vectors);
}

function fallbackQuestions(gameTitle: string): PersonaQuestion[] {
  return [
    {
      id: "preferred-experience",
      kind: "preference_metric",
      metric: "重視する体験",
      question: "ゲームで最も重視する体験は何ですか?",
      options: ["達成感", "爽快感", "収集/育成", "対人/協力", "物語/世界観", "短時間の気晴らし"],
      vectorHints: ["重視体験", "期待する報酬", "感情の方向"],
      weight: 1.2,
    },
    {
      id: "challenge-tolerance",
      kind: "preference_metric",
      metric: "難易度許容",
      question: "難しいステージや失敗が続く状況をどう受け止めますか?",
      options: ["燃える", "少しならよい", "報酬次第", "避けたい"],
      vectorHints: ["緊張", "達成感", "離脱リスク"],
      weight: 1,
    },
    {
      id: "spending-attitude",
      kind: "preference_metric",
      metric: "課金/報酬感度",
      question: "課金、ガチャ、報酬差についてどの程度気にしますか?",
      options: ["かなり気にする", "納得感があれば許容", "あまり気にしない", "むしろ強い報酬差が楽しい"],
      vectorHints: ["公平感", "報酬", "不満"],
      weight: 1,
    },
    {
      id: "play-frequency",
      kind: "game_usage",
      metric: "利用頻度",
      question: `${gameTitle}をどのくらいの頻度で遊ぶ想定ですか?`,
      options: ["毎日", "週に数回", "イベント時だけ", "ほぼ未プレイ"],
      vectorHints: ["継続", "習慣", "熱量"],
      weight: 1,
    },
    {
      id: "session-style",
      kind: "game_usage",
      metric: "プレイ単位",
      question: "1回のプレイではどのような遊び方を好みますか?",
      options: ["短時間で完結", "腰を据えて攻略", "友人と協力", "ながらプレイ"],
      vectorHints: ["テンポ", "没入", "社会性"],
      weight: 0.9,
    },
    {
      id: "first-impression",
      kind: "game_specific",
      metric: "初回体験",
      question: `${gameTitle}の初回体験で、何が分かりやすい/分かりにくいと感じそうですか?`,
      vectorHints: ["チュートリアル", "理解負荷", "初期離脱"],
      weight: 1.4,
    },
    {
      id: "favorite-system",
      kind: "game_specific",
      metric: "好むシステム",
      question: `${gameTitle}で特に好きになりそうな仕組みは何ですか?`,
      vectorHints: ["メカニクス適合", "満足要因", "継続理由"],
      weight: 1.2,
    },
    {
      id: "friction-system",
      kind: "game_specific",
      metric: "不満になりやすい点",
      question: `${gameTitle}で不満や離脱理由になりそうな仕組みは何ですか?`,
      vectorHints: ["摩擦", "負の感情", "改善要求"],
      weight: 1.3,
    },
  ];
}

function normalizeQuestion(raw: unknown, index: number): PersonaQuestion | null {
  if (!raw || typeof raw !== "object") return null;
  const obj = raw as Record<string, unknown>;
  const kind = KIND_VALUES.includes(obj.kind as PersonaQuestionKind)
    ? (obj.kind as PersonaQuestionKind)
    : null;
  const question = typeof obj.question === "string" ? obj.question.trim() : "";
  if (!kind || !question) return null;
  const metric =
    typeof obj.metric === "string" && obj.metric.trim()
      ? obj.metric.trim()
      : kind === "preference_metric"
        ? "嗜好指標"
        : kind === "game_usage"
          ? "利用/仕様理解"
          : "タイトル特化";
  return {
    id: safeQuestionId(obj.id, `${kind}-${index + 1}`),
    kind,
    metric,
    question,
    options: asStringArray(obj.options),
    vectorHints: asStringArray(obj.vectorHints ?? obj.vector_hints ?? obj.hints),
    weight: clampWeight(obj.weight),
  };
}

function ensureQuestionCoverage(questions: PersonaQuestion[], gameTitle: string, count: number): PersonaQuestion[] {
  const out = [...questions];
  const fallback = fallbackQuestions(gameTitle);
  for (const kind of KIND_VALUES) {
    if (!out.some((q) => q.kind === kind)) {
      const f = fallback.find((q) => q.kind === kind);
      if (f) out.push(f);
    }
  }
  const seen = new Set<string>();
  return out
    .filter((q) => {
      if (seen.has(q.id)) return false;
      seen.add(q.id);
      return true;
    })
    .slice(0, count);
}

function buildQuestionnairePrompt(args: BuildPersonaQuestionnaireArgs, count: number, baselineText: string) {
  const system =
    "あなたはゲームUXリサーチャーです。個人の嗜好と考え方からゲーム用ペルソナを作るための質問票を設計します。返答はJSON配列のみです。";
  const prompt =
    `# 対象ゲーム\n${args.gameTitle}\n\n` +
    `# 基本情報とユーザーの声\n${baselineText.slice(0, 6000)}\n\n` +
    `# ベクトル次元\n${VECTOR_DIMS.join(", ")}\n\n` +
    `${count}問の質問を作ってください。必ず以下を含めます。\n` +
    "- preference_metric: ゲーム嗜好を示す指標\n" +
    "- game_usage: 特定の仕様/ゲーム利用に関する質問\n" +
    "- game_specific: 対象ゲームに特化した質問\n\n" +
    "各要素は {\"id\":\"英数字ID\",\"kind\":\"preference_metric|game_usage|game_specific\",\"metric\":\"指標名\",\"question\":\"質問文\",\"options\":[\"任意の選択肢\"],\"vectorHints\":[\"回答をベクトル化する時の解釈語\"],\"weight\":1.0} の形にしてください。";
  return { system, prompt };
}

export async function buildPersonaQuestionnaire(args: BuildPersonaQuestionnaireArgs): Promise<PersonaQuestionnaire> {
  const warn = args.warn ?? ((m) => console.warn(`[persona-questionnaire/warn] ${m}`));
  const gameTitle = args.gameTitle.trim();
  if (!gameTitle) throw new Error("gameTitle is required");
  const count = clampCount(args.questionCount, 9);
  const baselineText = buildBaselineText(args);
  const base = baselineVector(args, baselineText);

  if (args.llm) {
    try {
      const { system, prompt } = buildQuestionnairePrompt(args, count, baselineText);
      const res = await args.llm.invoke({ system, prompt, model: args.model, maxTokens: 3000 });
      if (res.ok) {
        const arr = extractJsonArray(res.text);
        const questions = ensureQuestionCoverage(
          (arr ?? []).map(normalizeQuestion).filter((q): q is PersonaQuestion => q !== null),
          gameTitle,
          count
        );
        if (questions.length >= 3) {
          return {
            gameTitle,
            ...(args.gameSlug ? { gameSlug: args.gameSlug } : {}),
            ...(args.mechanicsContext ? { mechanicsContext: args.mechanicsContext } : {}),
            baselineText,
            baselineVector: base,
            questions,
            source: "llm",
          };
        }
      } else {
        warn(`質問票LLM生成失敗: ${res.error}`);
      }
    } catch (e) {
      warn(`質問票LLM生成例外: ${(e as Error).message}`);
    }
  }

  return {
    gameTitle,
    ...(args.gameSlug ? { gameSlug: args.gameSlug } : {}),
    ...(args.mechanicsContext ? { mechanicsContext: args.mechanicsContext } : {}),
    baselineText,
    baselineVector: base,
    questions: fallbackQuestions(gameTitle).slice(0, count),
    source: "fallback",
  };
}

function normalizeAnswers(input: PersonaAnswersInput): Map<string, unknown> {
  const out = new Map<string, unknown>();
  if (Array.isArray(input)) {
    for (const item of input) {
      if (item && typeof item.questionId === "string") out.set(item.questionId, item.answer);
    }
  } else {
    for (const [key, value] of Object.entries(input)) out.set(key, value);
  }
  return out;
}

function formatAnswer(value: unknown): string {
  if (Array.isArray(value)) return value.map(formatAnswer).filter(Boolean).join(" / ");
  if (value == null) return "";
  if (typeof value === "string") return value.trim();
  return String(value).trim();
}

function answerVectorText(question: PersonaQuestion, answer: string): string {
  const hints = question.vectorHints.length ? ` 解釈語: ${question.vectorHints.join(" / ")}。` : "";
  return `分類=${question.kind}。指標=${question.metric}。質問=${question.question}。回答=${answer}。${hints}`;
}

function topDeltas(delta: number[], direction: "positive" | "negative", limit = 5): PersonaVectorDelta[] {
  return delta
    .map((d, i) => ({ dim: VECTOR_DIMS[i] ?? `dim.${i}`, delta: +d.toFixed(4) }))
    .filter((d) => (direction === "positive" ? d.delta > 0 : d.delta < 0))
    .sort((a, b) => Math.abs(b.delta) - Math.abs(a.delta))
    .slice(0, limit);
}

export function analyzeQuestionnaireAnswers(
  questionnaire: PersonaQuestionnaire,
  answers: PersonaAnswersInput
): PersonaAnswerAnalysis {
  if (questionnaire.baselineVector.length !== DIM) {
    throw new Error(`baselineVector dim must be ${DIM}`);
  }
  const byId = normalizeAnswers(answers);
  const answerVectors: PersonaAnswerAnalysis["answerVectors"] = [];
  const vectors = [questionnaire.baselineVector];
  const weights = [0.75];

  for (const q of questionnaire.questions) {
    const answer = formatAnswer(byId.get(q.id));
    if (!answer) continue;
    const text = answerVectorText(q, answer);
    const vector = textToVector(text);
    vectors.push(vector);
    weights.push(q.weight);
    answerVectors.push({ questionId: q.id, weight: q.weight, vector, text });
  }

  if (answerVectors.length === 0) throw new Error("answers are required");
  const affectVector = averageVectors(vectors, weights);
  const responseVector = subtract(affectVector, questionnaire.baselineVector).map((v) => +v.toFixed(4));
  return {
    baselineVector: questionnaire.baselineVector,
    affectVector,
    responseVector,
    topPositiveDeltas: topDeltas(responseVector, "positive"),
    topNegativeDeltas: topDeltas(responseVector, "negative"),
    answerVectors,
  };
}

function fallbackMetadata(args: CreatePersonaFromAnswersArgs, analysis: PersonaAnswerAnalysis): PersonaMetadata {
  const answeredMetrics = analysis.answerVectors
    .map((a) => args.questionnaire.questions.find((q) => q.id === a.questionId)?.metric)
    .filter((v): v is string => !!v);
  const topDims = [...analysis.topPositiveDeltas, ...analysis.topNegativeDeltas].slice(0, 4).map((d) => d.dim);
  return {
    name: args.name?.trim() || `回答者#${randomUUID().slice(0, 6)}`,
    speechStyle: "自分の体験と納得感を軸に、具体例を交えて話す",
    traits: [...new Set(["回答型ペルソナ", ...answeredMetrics.slice(0, 4), ...topDims])].slice(0, 8),
    label: `${args.questionnaire.gameTitle} / 回答ベクトル生成 / ${answeredMetrics.slice(0, 3).join("・")}`,
  };
}

async function llmMetadata(args: CreatePersonaFromAnswersArgs, analysis: PersonaAnswerAnalysis): Promise<PersonaMetadata | null> {
  if (!args.llm) return null;
  const answerLines = analysis.answerVectors
    .map((a) => {
      const q = args.questionnaire.questions.find((item) => item.id === a.questionId);
      return q ? `- ${q.metric}: ${q.question} => ${a.text}` : `- ${a.text}`;
    })
    .join("\n");
  const prompt =
    `対象ゲーム: ${args.questionnaire.gameTitle}\n` +
    `回答:\n${answerLines.slice(0, 5000)}\n\n` +
    `ベクトル差分(正): ${analysis.topPositiveDeltas.map((d) => `${d.dim}:${d.delta}`).join(", ")}\n` +
    `ベクトル差分(負): ${analysis.topNegativeDeltas.map((d) => `${d.dim}:${d.delta}`).join(", ")}\n\n` +
    "この回答者を議論で使えるゲームユーザーペルソナとして要約してください。JSONのみで {\"name\":\"名前\",\"speechStyle\":\"話し方\",\"traits\":[\"特徴\"],\"label\":\"短いラベル\"} を返してください。";
  const res = await args.llm.invoke({ prompt, model: args.model, maxTokens: 1000 });
  if (!res.ok) return null;
  const arr = extractJsonArray(`[${res.text}]`);
  const obj = arr?.[0];
  if (!obj || typeof obj !== "object") return null;
  const raw = obj as Record<string, unknown>;
  const name = typeof raw.name === "string" && raw.name.trim() ? raw.name.trim() : "";
  return {
    name: args.name?.trim() || name || `回答者#${randomUUID().slice(0, 6)}`,
    speechStyle:
      typeof raw.speechStyle === "string" && raw.speechStyle.trim()
        ? raw.speechStyle.trim()
        : typeof raw.speech_style === "string"
          ? raw.speech_style.trim()
          : "自分の体験と納得感を軸に話す",
    traits: asStringArray(raw.traits).slice(0, 8),
    label: typeof raw.label === "string" && raw.label.trim() ? raw.label.trim() : args.questionnaire.gameTitle,
  };
}

export async function createPersonaFromQuestionnaireAnswers(
  args: CreatePersonaFromAnswersArgs
): Promise<CreatePersonaFromAnswersResult> {
  const warn = args.warn ?? ((m) => console.warn(`[persona-questionnaire/warn] ${m}`));
  const analysis = analyzeQuestionnaireAnswers(args.questionnaire, args.answers);
  let metadata: PersonaMetadata | null = null;
  try {
    metadata = await llmMetadata(args, analysis);
  } catch (e) {
    warn(`回答ペルソナ要約LLM失敗: ${(e as Error).message}`);
  }
  metadata ??= fallbackMetadata(args, analysis);
  if (metadata.traits.length === 0) metadata = { ...metadata, traits: fallbackMetadata(args, analysis).traits };

  const persona: PoolPersona = {
    id: randomUUID(),
    name: metadata.name,
    role: args.role ?? "opinion",
    speechStyle: metadata.speechStyle,
    traits: metadata.traits,
    affectVector: analysis.affectVector,
    origin: "generated",
    parentIds: [],
    learningSource: "questionnaire",
    label: metadata.label,
  };
  if (args.persist !== false) insertPoolPersona(persona);
  return { persona, analysis, saved: args.persist !== false };
}
