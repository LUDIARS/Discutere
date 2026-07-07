import { randomUUID } from "node:crypto";
import type { LLMClient } from "../persona-engine/llm/client.js";
import type { FlowRole } from "./personas.js";
import { extractJsonArray } from "./mechanic-extract.js";
import { insertPoolPersona, type PoolPersona } from "./persona-pool.js";
import { averageVectors } from "./persona-synthesize.js";
import { DIM, VECTOR_DIMS, subtract, textToVector } from "./sentiment-vector.js";

export type PersonaQuestionKind = "preference_metric" | "game_usage" | "game_specific";

export const PREFERENCE_AXES = [
  { id: "mtg.timmy", label: "Timmy/Tammy", description: "大きな体験、派手さ、爽快感、感情的な盛り上がりを好む" },
  { id: "mtg.johnny", label: "Johnny/Jenny", description: "独自性、コンボ、創造的な攻略、自己表現を好む" },
  { id: "mtg.spike", label: "Spike", description: "勝率、最適化、上達、競技性、効率を好む" },
  { id: "style.achiever", label: "Achiever", description: "目標達成、進捗、実績、報酬を好む" },
  { id: "style.explorer", label: "Explorer", description: "発見、試行錯誤、隠し要素、世界理解を好む" },
  { id: "style.socializer", label: "Socializer", description: "協力、交流、共有体験を好む" },
  { id: "style.competitor", label: "Competitor", description: "対人、ランキング、他者比較を好む" },
  { id: "style.collector", label: "Collector", description: "キャラ、図鑑、装備、コレクションを好む" },
  { id: "style.narrative", label: "Narrative", description: "物語、世界観、キャラクター文脈を好む" },
  { id: "style.relaxation", label: "Relaxation", description: "短時間、気晴らし、低負荷な遊びを好む" },
  { id: "style.mastery", label: "Mastery", description: "練習、理解、難所突破、技術上達を好む" },
  { id: "style.onboarding_need", label: "Onboarding Need", description: "丁寧な説明、安心できる導線、失敗時の補助を求める" },
  { id: "style.autonomy", label: "Autonomy", description: "自分で触って覚える、自由に試す、自走して攻略することを好む" },
  { id: "style.routine_tolerance", label: "Routine Tolerance", description: "日課、ログイン、周回、時限イベントを受け入れやすい" },
  { id: "style.monetization_sensitivity", label: "Monetization Sensitivity", description: "課金圧、ガチャ、報酬差、公平性に敏感" },
] as const;

export type PersonaPreferenceAxis = (typeof PREFERENCE_AXES)[number]["id"];
export type PersonaPreferenceScoreMap = Partial<Record<PersonaPreferenceAxis, number>>;

export interface PersonaQuestionOption {
  label: string;
  scores: PersonaPreferenceScoreMap;
}

export interface PersonaQuestionTemplate {
  id: string;
  kind: PersonaQuestionKind;
  metric: string;
  essence: string;
  questionTemplate: string;
  options: PersonaQuestionOption[];
  vectorHints: string[];
  weight: number;
}

export interface PersonaQuestion {
  id: string;
  templateId?: string;
  kind: PersonaQuestionKind;
  metric: string;
  essence?: string;
  question: string;
  options?: string[];
  scoring?: Record<string, PersonaPreferenceScoreMap>;
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

export interface PersonaPreferenceDelta {
  axis: PersonaPreferenceAxis;
  label: string;
  score: number;
}

export interface PersonaAnswerAnalysis {
  baselineVector: number[];
  affectVector: number[];
  responseVector: number[];
  preferenceVector: number[];
  preferenceScores: Partial<Record<PersonaPreferenceAxis, number>>;
  topPreferenceAxes: PersonaPreferenceDelta[];
  topPositiveDeltas: PersonaVectorDelta[];
  topNegativeDeltas: PersonaVectorDelta[];
  answerVectors: Array<{ questionId: string; weight: number; vector: number[]; text: string }>;
}

export interface CreatePersonaFromAnswersArgs {
  questionnaire: PersonaQuestionnaire;
  answers: PersonaAnswersInput;
  additionalInfo?: string;
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

export interface AnalyzeQuestionnaireAnswersOptions {
  additionalInfo?: string;
}

interface PersonaMetadata {
  name: string;
  speechStyle: string;
  traits: string[];
  label: string;
}

const KIND_VALUES: readonly PersonaQuestionKind[] = ["preference_metric", "game_usage", "game_specific"];
const PREFERENCE_AXIS_IDS = new Set<string>(PREFERENCE_AXES.map((a) => a.id));

function clampCount(value: number | undefined, fallback: number): number {
  if (value == null || !Number.isFinite(value)) return fallback;
  return Math.max(3, Math.min(24, Math.floor(value)));
}

function clampWeight(value: unknown): number {
  const n = typeof value === "number" ? value : Number(value);
  if (!Number.isFinite(n)) return 1;
  return Math.max(0.1, Math.min(3, +n.toFixed(2)));
}

function clampPreferenceScore(value: unknown): number | null {
  const n = typeof value === "number" ? value : Number(value);
  if (!Number.isFinite(n)) return null;
  return Math.max(-1, Math.min(1, +n.toFixed(3)));
}

function asStringArray(value: unknown): string[] {
  if (!Array.isArray(value)) return [];
  return value.map((v) => (typeof v === "string" ? v.trim() : "")).filter(Boolean).slice(0, 8);
}

function normalizePreferenceScoreMap(value: unknown): PersonaPreferenceScoreMap {
  if (!value || typeof value !== "object") return {};
  const out: PersonaPreferenceScoreMap = {};
  for (const [axis, rawScore] of Object.entries(value as Record<string, unknown>)) {
    if (!PREFERENCE_AXIS_IDS.has(axis)) continue;
    const score = clampPreferenceScore(rawScore);
    if (score !== null && score !== 0) out[axis as PersonaPreferenceAxis] = score;
  }
  return out;
}

function normalizeQuestionScoring(value: unknown): Record<string, PersonaPreferenceScoreMap> | undefined {
  if (!value || typeof value !== "object") return undefined;
  const out: Record<string, PersonaPreferenceScoreMap> = {};
  for (const [label, rawScores] of Object.entries(value as Record<string, unknown>)) {
    const key = label.trim();
    if (!key) continue;
    const scores = normalizePreferenceScoreMap(rawScores);
    if (Object.keys(scores).length > 0) out[key] = scores;
  }
  return Object.keys(out).length > 0 ? out : undefined;
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

function option(label: string, scores: PersonaPreferenceScoreMap): PersonaQuestionOption {
  return { label, scores };
}

function renderQuestionTemplate(template: PersonaQuestionTemplate, gameTitle: string): PersonaQuestion {
  const scoring = Object.fromEntries(template.options.map((o) => [o.label, o.scores]));
  return {
    id: template.id,
    templateId: template.id,
    kind: template.kind,
    metric: template.metric,
    essence: template.essence,
    question: template.questionTemplate.replaceAll("{gameTitle}", gameTitle),
    options: template.options.map((o) => o.label),
    scoring,
    vectorHints: [...template.vectorHints],
    weight: template.weight,
  };
}

export function genericPersonaQuestionTemplates(): PersonaQuestionTemplate[] {
  return [
    {
      id: "primary-motivation",
      kind: "preference_metric",
      metric: "主目的/動機",
      essence: "ゲームに期待する中核価値を Timmy/Johnny/Spike と汎用プレイスタイルへ写像する",
      questionTemplate: "ゲームを遊ぶ時、最も満たしたい目的は何ですか?",
      options: [
        option("達成感", { "mtg.spike": 0.35, "style.achiever": 0.9, "style.mastery": 0.45 }),
        option("爽快感", { "mtg.timmy": 0.9, "style.relaxation": 0.45 }),
        option("収集/育成", { "style.collector": 0.9, "style.achiever": 0.45 }),
        option("対人/協力", { "style.socializer": 0.65, "style.competitor": 0.45 }),
        option("物語/世界観", { "style.narrative": 0.9, "style.explorer": 0.35 }),
        option("短時間の気晴らし", { "style.relaxation": 0.9, "style.routine_tolerance": 0.25 }),
      ],
      vectorHints: ["重視体験", "期待する報酬", "継続理由"],
      weight: 1.2,
    },
    {
      id: "play-frequency",
      kind: "game_usage",
      metric: "利用頻度",
      essence: "対象ゲームへの生活内接続度と日課許容を測る",
      questionTemplate: "{gameTitle}をどのくらいの頻度で遊ぶ想定ですか?",
      options: [
        option("毎日", { "style.routine_tolerance": 0.9, "style.achiever": 0.35 }),
        option("週に数回", { "style.routine_tolerance": 0.35 }),
        option("イベント時だけ", { "style.routine_tolerance": -0.2, "style.collector": 0.3 }),
        option("ほぼ未プレイ", { "style.routine_tolerance": -0.5, "style.onboarding_need": 0.35 }),
      ],
      vectorHints: ["継続", "習慣", "熱量"],
      weight: 1,
    },
    {
      id: "first-session-learning",
      kind: "game_specific",
      metric: "初回理解",
      essence: "初回体験で安心に必要な情報を測り、タイトル特化の導線質問へ変換する",
      questionTemplate: "{gameTitle}の初回プレイで、最初に理解できていると安心することは何ですか?",
      options: [
        option("基本操作", { "style.onboarding_need": 0.8, "style.autonomy": -0.2 }),
        option("勝ち方", { "mtg.spike": 0.45, "style.mastery": 0.55 }),
        option("育成/強化", { "style.achiever": 0.55, "style.collector": 0.35 }),
        option("報酬の価値", { "style.achiever": 0.45, "style.monetization_sensitivity": 0.35 }),
        option("遊ぶ目的", { "style.onboarding_need": 0.6, "style.narrative": 0.25 }),
        option("後で確認できる導線", { "style.onboarding_need": 0.65, "style.autonomy": 0.25 }),
      ],
      vectorHints: ["チュートリアル", "理解負荷", "初回定着"],
      weight: 1.4,
    },
    {
      id: "challenge-tolerance",
      kind: "preference_metric",
      metric: "難易度許容",
      essence: "難度、失敗、上達圧への反応を Spike/Mastery/Relaxation に分解する",
      questionTemplate: "難しいステージや失敗が続く状況をどう受け止めますか?",
      options: [
        option("燃える", { "mtg.spike": 0.75, "style.mastery": 0.9, "style.relaxation": -0.35 }),
        option("少しならよい", { "style.mastery": 0.25 }),
        option("報酬次第", { "style.achiever": 0.55, "style.mastery": 0.15 }),
        option("避けたい", { "style.relaxation": 0.65, "style.mastery": -0.45 }),
      ],
      vectorHints: ["緊張", "達成感", "離脱リスク"],
      weight: 1,
    },
    {
      id: "session-style",
      kind: "game_usage",
      metric: "プレイ単位",
      essence: "1セッションの密度、没入、社交性、軽さを測る",
      questionTemplate: "1回のプレイではどのような遊び方を好みますか?",
      options: [
        option("短時間で完結", { "style.relaxation": 0.65, "style.routine_tolerance": 0.35 }),
        option("腰を据えて攻略", { "mtg.spike": 0.35, "style.mastery": 0.7, "style.explorer": 0.25 }),
        option("友人と協力", { "style.socializer": 0.9 }),
        option("ながらプレイ", { "style.relaxation": 0.7, "style.mastery": -0.25 }),
      ],
      vectorHints: ["テンポ", "没入", "社会性"],
      weight: 0.9,
    },
    {
      id: "expected-friction",
      kind: "game_specific",
      metric: "不安/離脱予兆",
      essence: "対象ゲームで離脱を起こす摩擦を、説明/作業/難度/報酬/課金/対人不安に分ける",
      questionTemplate: "{gameTitle}で不満や離脱理由になりそうだと感じる点は何ですか?",
      options: [
        option("説明不足", { "style.onboarding_need": 0.85, "style.autonomy": -0.25 }),
        option("作業感", { "style.routine_tolerance": -0.65, "style.relaxation": -0.25 }),
        option("難しすぎる", { "style.mastery": -0.45, "style.onboarding_need": 0.35 }),
        option("報酬が渋い", { "style.achiever": 0.55, "style.routine_tolerance": -0.25 }),
        option("課金圧", { "style.monetization_sensitivity": 0.9 }),
        option("人に迷惑をかける不安", { "style.socializer": -0.25, "style.onboarding_need": 0.45 }),
      ],
      vectorHints: ["摩擦", "負の感情", "改善要求"],
      weight: 1.3,
    },
    {
      id: "learning-style",
      kind: "preference_metric",
      metric: "学習スタイル",
      essence: "説明主導か実践主導か、自己探索か外部攻略かを測る",
      questionTemplate: "新しいルールや操作は、どのように覚えるのが好きですか?",
      options: [
        option("細かく説明してほしい", { "style.onboarding_need": 0.9, "style.autonomy": -0.35 }),
        option("触りながら覚えたい", { "style.autonomy": 0.75, "style.onboarding_need": -0.15 }),
        option("困った時だけ見たい", { "style.autonomy": 0.55, "style.onboarding_need": 0.15 }),
        option("攻略情報を自分で探したい", { "mtg.johnny": 0.35, "style.explorer": 0.65, "style.autonomy": 0.7 }),
      ],
      vectorHints: ["理解負荷", "自律性", "チュートリアル適性"],
      weight: 1.2,
    },
    {
      id: "session-length",
      kind: "game_usage",
      metric: "可処分時間",
      essence: "生活上のプレイ時間制約を測り、短時間適性や深い攻略適性へつなげる",
      questionTemplate: "普段、1回のゲームにどのくらい時間を使いやすいですか?",
      options: [
        option("5分以内", { "style.relaxation": 0.75, "style.routine_tolerance": 0.25, "style.mastery": -0.25 }),
        option("10-20分", { "style.relaxation": 0.35 }),
        option("30分以上", { "style.mastery": 0.45, "style.explorer": 0.3 }),
        option("時間が取れる時だけ長く遊ぶ", { "style.explorer": 0.35, "style.routine_tolerance": -0.25 }),
      ],
      vectorHints: ["短時間適性", "継続負荷", "生活導線"],
      weight: 0.9,
    },
    {
      id: "favorite-system-expectation",
      kind: "game_specific",
      metric: "期待する仕組み",
      essence: "タイトル固有メカニクスへの期待を既存の嗜好軸へ写像する",
      questionTemplate: "{gameTitle}で特に好きになりそうな仕組みは何ですか?",
      options: [
        option("操作の気持ちよさ", { "mtg.timmy": 0.55, "style.relaxation": 0.35 }),
        option("編成/戦略", { "mtg.johnny": 0.75, "mtg.spike": 0.35, "style.mastery": 0.45 }),
        option("キャラ収集", { "style.collector": 0.9 }),
        option("育成", { "style.achiever": 0.65, "style.collector": 0.35 }),
        option("協力/対人", { "style.socializer": 0.55, "style.competitor": 0.45 }),
        option("イベント", { "style.routine_tolerance": 0.35, "style.collector": 0.25 }),
      ],
      vectorHints: ["メカニクス適合", "満足要因", "継続理由"],
      weight: 1.2,
    },
    {
      id: "reward-rhythm",
      kind: "preference_metric",
      metric: "報酬テンポ",
      essence: "報酬頻度と報酬依存度を Achiever/Timmy/Autonomy に分ける",
      questionTemplate: "報酬や成長は、どのようなテンポだと続けやすいですか?",
      options: [
        option("毎回小さな報酬が欲しい", { "style.achiever": 0.55, "style.routine_tolerance": 0.35 }),
        option("節目で大きな報酬が欲しい", { "mtg.timmy": 0.55, "style.achiever": 0.35 }),
        option("努力量に比例してほしい", { "mtg.spike": 0.45, "style.achiever": 0.55 }),
        option("報酬より遊び自体を重視", { "mtg.johnny": 0.35, "style.autonomy": 0.45, "style.achiever": -0.25 }),
      ],
      vectorHints: ["報酬", "成長実感", "継続"],
      weight: 1.1,
    },
    {
      id: "return-trigger",
      kind: "game_usage",
      metric: "復帰きっかけ",
      essence: "復帰動機をライブ運用/社会性/収集/改善/気晴らしへ分解する",
      questionTemplate: "一度離れたゲームに戻るきっかけになりやすいものは何ですか?",
      options: [
        option("新イベント", { "style.routine_tolerance": 0.35, "style.explorer": 0.25 }),
        option("友人の誘い", { "style.socializer": 0.8 }),
        option("好きなキャラ/報酬", { "style.collector": 0.65, "style.achiever": 0.35 }),
        option("改善アップデート", { "style.autonomy": 0.25, "style.mastery": 0.25 }),
        option("暇な時間", { "style.relaxation": 0.65 }),
      ],
      vectorHints: ["復帰", "ライブ運用", "外部動機"],
      weight: 1,
    },
    {
      id: "mechanic-clarity",
      kind: "game_specific",
      metric: "仕様理解",
      essence: "対象ゲームで理解不足が楽しさを阻害する仕様領域を特定する",
      questionTemplate: "{gameTitle}で「分からないと楽しみにくい」と感じそうな仕様は何ですか?",
      options: [
        option("操作", { "style.onboarding_need": 0.6 }),
        option("勝敗条件", { "mtg.spike": 0.35, "style.onboarding_need": 0.45 }),
        option("育成", { "style.achiever": 0.45, "style.collector": 0.25 }),
        option("編成/相性", { "mtg.johnny": 0.55, "mtg.spike": 0.35 }),
        option("報酬", { "style.achiever": 0.35, "style.monetization_sensitivity": 0.25 }),
        option("マルチ/協力", { "style.socializer": 0.55, "style.onboarding_need": 0.25 }),
      ],
      vectorHints: ["仕様理解", "説明不足", "不安"],
      weight: 1.2,
    },
    {
      id: "spending-attitude",
      kind: "preference_metric",
      metric: "課金/報酬感度",
      essence: "マネタイズと公平性への感度を測る",
      questionTemplate: "課金、ガチャ、報酬差についてどの程度気にしますか?",
      options: [
        option("かなり気にする", { "style.monetization_sensitivity": 0.9 }),
        option("納得感があれば許容", { "style.monetization_sensitivity": 0.35 }),
        option("あまり気にしない", { "style.monetization_sensitivity": -0.35 }),
        option("むしろ強い報酬差が楽しい", { "mtg.timmy": 0.3, "style.achiever": 0.35, "style.monetization_sensitivity": -0.55 }),
      ],
      vectorHints: ["公平感", "報酬", "不満"],
      weight: 1,
    },
    {
      id: "social-comfort",
      kind: "game_usage",
      metric: "ソーシャル距離",
      essence: "ソロ/協力/固定コミュニティ/競争の好みを測る",
      questionTemplate: "他プレイヤーとの関わりはどの程度あると心地よいですか?",
      options: [
        option("一人で完結したい", { "style.socializer": -0.55, "style.autonomy": 0.45 }),
        option("ゆるく協力したい", { "style.socializer": 0.65, "style.competitor": -0.15 }),
        option("固定メンバーで遊びたい", { "style.socializer": 0.9 }),
        option("競争/ランキングが欲しい", { "style.competitor": 0.9, "mtg.spike": 0.5 }),
      ],
      vectorHints: ["社会性", "協力", "競争ストレス"],
      weight: 1,
    },
    {
      id: "content-interest",
      kind: "game_specific",
      metric: "関心コンテンツ",
      essence: "対象ゲームで最初に向かうコンテンツから関心軸を推定する",
      questionTemplate: "{gameTitle}で最初に触ってみたいコンテンツやモードは何ですか?",
      options: [
        option("ストーリー/通常攻略", { "style.narrative": 0.55, "style.onboarding_need": 0.2 }),
        option("イベント", { "style.routine_tolerance": 0.35, "style.collector": 0.2 }),
        option("高難度", { "mtg.spike": 0.65, "style.mastery": 0.8 }),
        option("協力/マルチ", { "style.socializer": 0.75 }),
        option("育成/編成", { "mtg.johnny": 0.45, "style.achiever": 0.45 }),
        option("コレクション", { "style.collector": 0.9 }),
      ],
      vectorHints: ["初期導線", "関心対象", "探索"],
      weight: 1,
    },
    {
      id: "complexity-depth",
      kind: "preference_metric",
      metric: "複雑さ/奥深さ",
      essence: "複雑性を楽しむか、段階的開示を求めるかを測る",
      questionTemplate: "複雑なシステムや最適化要素をどう感じますか?",
      options: [
        option("深く考えるほど楽しい", { "mtg.johnny": 0.65, "mtg.spike": 0.45, "style.mastery": 0.65 }),
        option("基本だけ分かればよい", { "style.relaxation": 0.45, "style.mastery": -0.35 }),
        option("段階的なら歓迎", { "style.onboarding_need": 0.45, "style.mastery": 0.25 }),
        option("面倒に感じやすい", { "style.relaxation": 0.55, "style.mastery": -0.55, "mtg.johnny": -0.35 }),
      ],
      vectorHints: ["戦略性", "理解負荷", "奥深さ"],
      weight: 1.1,
    },
    {
      id: "notification-tolerance",
      kind: "game_usage",
      metric: "通知/時限要素",
      essence: "日課/時限/ログイン導線への受容度を測る",
      questionTemplate: "通知、ログインボーナス、時限イベントのような日課要素をどう受け止めますか?",
      options: [
        option("習慣化しやすくてよい", { "style.routine_tolerance": 0.9 }),
        option("報酬次第", { "style.routine_tolerance": 0.25, "style.achiever": 0.35 }),
        option("急かされると負担", { "style.routine_tolerance": -0.65, "style.relaxation": 0.25 }),
        option("ほとんど不要", { "style.routine_tolerance": -0.85, "style.autonomy": 0.35 }),
      ],
      vectorHints: ["日課", "継続圧", "負担"],
      weight: 1,
    },
    {
      id: "identity-attachment",
      kind: "preference_metric",
      metric: "愛着形成",
      essence: "長期継続を支える愛着対象を測る",
      questionTemplate: "ゲーム内で愛着を持ちやすいものは何ですか?",
      options: [
        option("キャラクター", { "style.collector": 0.45, "style.narrative": 0.45 }),
        option("自分の成長", { "style.achiever": 0.65, "style.mastery": 0.45 }),
        option("コレクション", { "style.collector": 0.9 }),
        option("仲間/コミュニティ", { "style.socializer": 0.85 }),
        option("世界観", { "style.narrative": 0.85, "style.explorer": 0.25 }),
        option("実績", { "style.achiever": 0.75, "mtg.spike": 0.25 }),
      ],
      vectorHints: ["愛着", "所有感", "長期継続"],
      weight: 1,
    },
    {
      id: "failure-recovery",
      kind: "game_usage",
      metric: "失敗時の復帰",
      essence: "失敗後に再挑戦へ戻る条件を測る",
      questionTemplate: "失敗した時、次も挑戦しようと思える条件は何ですか?",
      options: [
        option("原因が分かる", { "mtg.spike": 0.45, "style.mastery": 0.75 }),
        option("すぐ再挑戦できる", { "style.mastery": 0.45, "style.relaxation": 0.25 }),
        option("少し報酬が残る", { "style.achiever": 0.5, "style.routine_tolerance": 0.25 }),
        option("別の進め方がある", { "mtg.johnny": 0.6, "style.explorer": 0.45, "style.autonomy": 0.35 }),
        option("仲間に助けてもらえる", { "style.socializer": 0.65, "style.onboarding_need": 0.3 }),
      ],
      vectorHints: ["再挑戦", "学習", "離脱防止"],
      weight: 1.1,
    },
    {
      id: "discussion-viewpoint",
      kind: "preference_metric",
      metric: "議論観点",
      essence: "議論時にどの評価軸から発話しやすいかを測る",
      questionTemplate: "ゲームについて意見を言う時、どの観点を重視しがちですか?",
      options: [
        option("初心者の分かりやすさ", { "style.onboarding_need": 0.8 }),
        option("上達/攻略の深さ", { "mtg.spike": 0.5, "style.mastery": 0.75 }),
        option("公平性", { "style.monetization_sensitivity": 0.65, "style.competitor": 0.25 }),
        option("継続しやすさ", { "style.routine_tolerance": 0.35, "style.relaxation": 0.35 }),
        option("友人と遊ぶ楽しさ", { "style.socializer": 0.85 }),
        option("運営への信頼", { "style.monetization_sensitivity": 0.45, "style.routine_tolerance": 0.2 }),
      ],
      vectorHints: ["議論材料", "評価軸", "価値観"],
      weight: 1.2,
    },
  ];
}

export function genericPersonaQuestionBank(gameTitle = "対象ゲーム"): PersonaQuestion[] {
  return genericPersonaQuestionTemplates().map((template) => renderQuestionTemplate(template, gameTitle));
}

function fallbackQuestions(gameTitle: string): PersonaQuestion[] {
  return genericPersonaQuestionBank(gameTitle);
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
    templateId:
      typeof obj.templateId === "string" && obj.templateId.trim()
        ? obj.templateId.trim()
        : typeof obj.template_id === "string" && obj.template_id.trim()
          ? obj.template_id.trim()
          : undefined,
    kind,
    metric,
    essence:
      typeof obj.essence === "string" && obj.essence.trim()
        ? obj.essence.trim()
        : undefined,
    question,
    options: asStringArray(obj.options),
    scoring: normalizeQuestionScoring(obj.scoring ?? obj.scores),
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
  const genericGuide = genericPersonaQuestionBank(args.gameTitle)
    .map((q) => `- ${q.kind} / ${q.metric}: ${q.essence ?? q.question}`)
    .join("\n");
  const axisGuide = PREFERENCE_AXES.map((a) => `- ${a.id}: ${a.label} (${a.description})`).join("\n");
  const system =
    "あなたはゲームUXリサーチャーです。個人の嗜好と考え方からゲーム用ペルソナを作るための質問票を設計します。返答はJSON配列のみです。";
  const prompt =
    `# 対象ゲーム\n${args.gameTitle}\n\n` +
    `# 基本情報とユーザーの声\n${baselineText.slice(0, 6000)}\n\n` +
    `# ベクトル次元\n${VECTOR_DIMS.join(", ")}\n\n` +
    `# 嗜好軸\n${axisGuide}\n\n` +
    `# 汎用質問集の観点\n${genericGuide}\n\n` +
    `${count}問の質問を作ってください。必ず以下を含めます。\n` +
    "- preference_metric: ゲーム嗜好を示す指標\n" +
    "- game_usage: 特定の仕様/ゲーム利用に関する質問\n" +
    "- game_specific: 対象ゲームに特化した質問\n\n" +
    "各要素は {\"id\":\"英数字ID\",\"kind\":\"preference_metric|game_usage|game_specific\",\"metric\":\"指標名\",\"essence\":\"質問の本質\",\"question\":\"質問文\",\"options\":[\"選択肢\"],\"scoring\":{\"選択肢\":{\"mtg.timmy\":0.5}},\"vectorHints\":[\"回答をベクトル化する時の解釈語\"],\"weight\":1.0} の形にしてください。scoring の値は -1.0 から 1.0 です。";
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

function mergePreferenceMaps(maps: PersonaPreferenceScoreMap[]): PersonaPreferenceScoreMap {
  if (maps.length === 0) return {};
  const sums = new Map<PersonaPreferenceAxis, number>();
  const counts = new Map<PersonaPreferenceAxis, number>();
  for (const map of maps) {
    for (const [axis, rawScore] of Object.entries(map) as Array<[PersonaPreferenceAxis, number]>) {
      const score = clampPreferenceScore(rawScore);
      if (score === null) continue;
      sums.set(axis, (sums.get(axis) ?? 0) + score);
      counts.set(axis, (counts.get(axis) ?? 0) + 1);
    }
  }
  const out: PersonaPreferenceScoreMap = {};
  for (const [axis, sum] of sums.entries()) {
    const count = counts.get(axis) ?? 1;
    out[axis] = +(sum / count).toFixed(4);
  }
  return out;
}

function scoreAnswer(question: PersonaQuestion, answer: string): PersonaPreferenceScoreMap {
  if (!question.scoring) return {};
  const exact = question.scoring[answer];
  if (exact) return exact;

  const parts = answer
    .split(/\s*[/／]\s*|\r?\n/)
    .map((s) => s.trim())
    .filter(Boolean);
  const matched = parts.map((part) => question.scoring?.[part]).filter((v): v is PersonaPreferenceScoreMap => !!v);
  if (matched.length > 0) return mergePreferenceMaps(matched);

  const included = Object.entries(question.scoring)
    .filter(([label]) => answer.includes(label))
    .map(([, scores]) => scores);
  return mergePreferenceMaps(included);
}

function analyzePreferenceScores(
  questions: readonly PersonaQuestion[],
  answersById: Map<string, unknown>
): {
  preferenceScores: Partial<Record<PersonaPreferenceAxis, number>>;
  preferenceVector: number[];
  topPreferenceAxes: PersonaPreferenceDelta[];
} {
  const sums = new Map<PersonaPreferenceAxis, number>();
  const weights = new Map<PersonaPreferenceAxis, number>();

  for (const q of questions) {
    const answer = formatAnswer(answersById.get(q.id));
    if (!answer) continue;
    const scores = scoreAnswer(q, answer);
    for (const [axis, rawScore] of Object.entries(scores) as Array<[PersonaPreferenceAxis, number]>) {
      const score = clampPreferenceScore(rawScore);
      if (score === null) continue;
      sums.set(axis, (sums.get(axis) ?? 0) + score * q.weight);
      weights.set(axis, (weights.get(axis) ?? 0) + q.weight);
    }
  }

  const preferenceScores: Partial<Record<PersonaPreferenceAxis, number>> = {};
  for (const axis of PREFERENCE_AXES.map((a) => a.id)) {
    const weight = weights.get(axis);
    if (!weight) continue;
    preferenceScores[axis] = +((sums.get(axis) ?? 0) / weight).toFixed(4);
  }
  const preferenceVector = PREFERENCE_AXES.map((axis) => preferenceScores[axis.id] ?? 0);
  const topPreferenceAxes = PREFERENCE_AXES.map((axis) => ({
    axis: axis.id,
    label: axis.label,
    score: preferenceScores[axis.id] ?? 0,
  }))
    .filter((item) => item.score !== 0)
    .sort((a, b) => Math.abs(b.score) - Math.abs(a.score))
    .slice(0, 8);
  return { preferenceScores, preferenceVector, topPreferenceAxes };
}

export function analyzeQuestionnaireAnswers(
  questionnaire: PersonaQuestionnaire,
  answers: PersonaAnswersInput,
  options: AnalyzeQuestionnaireAnswersOptions = {}
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

  const additionalInfo = options.additionalInfo?.trim();
  if (additionalInfo) {
    const text = `追加情報。${additionalInfo}`;
    const vector = textToVector(text);
    vectors.push(vector);
    weights.push(0.8);
    answerVectors.push({ questionId: "__additional_info", weight: 0.8, vector, text });
  }

  if (answerVectors.length === 0) throw new Error("answers are required");
  const affectVector = averageVectors(vectors, weights);
  const responseVector = subtract(affectVector, questionnaire.baselineVector).map((v) => +v.toFixed(4));
  const preferenceAnalysis = analyzePreferenceScores(questionnaire.questions, byId);
  return {
    baselineVector: questionnaire.baselineVector,
    affectVector,
    responseVector,
    preferenceVector: preferenceAnalysis.preferenceVector,
    preferenceScores: preferenceAnalysis.preferenceScores,
    topPreferenceAxes: preferenceAnalysis.topPreferenceAxes,
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
  const topPreferenceLabels = analysis.topPreferenceAxes.slice(0, 4).map((d) => d.label);
  const extraTraits = args.additionalInfo?.trim() ? ["追加情報あり"] : [];
  return {
    name: args.name?.trim() || `回答者#${randomUUID().slice(0, 6)}`,
    speechStyle: "自分の体験と納得感を軸に、具体例を交えて話す",
    traits: [...new Set(["回答型ペルソナ", ...extraTraits, ...topPreferenceLabels, ...answeredMetrics.slice(0, 3), ...topDims])].slice(0, 8),
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
    (args.additionalInfo?.trim() ? `追加情報:\n${args.additionalInfo.trim().slice(0, 2000)}\n\n` : "") +
    `嗜好軸: ${analysis.topPreferenceAxes.map((d) => `${d.label}:${d.score}`).join(", ")}\n` +
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
  const analysis = analyzeQuestionnaireAnswers(args.questionnaire, args.answers, {
    additionalInfo: args.additionalInfo,
  });
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
