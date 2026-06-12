/**
 * RESTRUCTURE パイプライン共有型定義。
 * spec/RESTRUCTURE.md §0-§2 に対応する中間表現とドメイン型。
 *
 * Phase 0 方針: DB schema 変更なし。resource edge / role / evaluation は
 * sidecar JSON で表現 (spec §5 非ゴール)。
 */

// ── ①嗜好プロファイル ────────────────────────────────────────────────────────

/** SENTIMENT 固定 20 次元ベクトル (0..1)。spec/crawler/SENTIMENT.md に準拠。 */
export type SentimentVector = [
  number, number, number, number, number, // emo: valence arousal joy trust fear
  number, number, number, number, number, // emo: surprise sadness disgust anger anticipation
  number, number, number, number, number, // asp: fun difficulty content price_value performance
  number, number, number,                 // asp: story graphics replayability
  number, number,                         // meta: positive_ratio volume_log
];

export const SENTIMENT_DIMS = [
  "emo.valence", "emo.arousal", "emo.joy", "emo.trust", "emo.fear",
  "emo.surprise", "emo.sadness", "emo.disgust", "emo.anger", "emo.anticipation",
  "asp.fun", "asp.difficulty", "asp.content", "asp.price_value", "asp.performance",
  "asp.story", "asp.graphics", "asp.replayability",
  "meta.positive_ratio", "meta.volume_log",
] as const;

export interface PreferenceProfile {
  /** "global" | "genre:<name>" | "mechanic:<name>" */
  scope: string;
  vector: SentimentVector;
  sampleCount: number;
  /** 出所 (source + URL)。個人は仮名化済みのものだけ含む。 */
  sources: string[];
}

/** step1 出力: data/preferences/<scope>.json */
export interface Step1Output {
  profiles: PreferenceProfile[];
}

// ── ②取り込み中間表現 ────────────────────────────────────────────────────────

export interface MechanicMid {
  id: string;
  name: string;
  description?: string;
  intends?: string;
  intendedAffect?: string;
  playContext?: string;
}

export interface GameIntake {
  slug: string;
  title: string;
  genre?: string;
  /** "kg" = KG md frontmatter から取得, "spec" = LLM 類推 */
  source: "kg" | "spec";
  mechanics: MechanicMid[];
  /** .sentiment.json の overall/affects 等。存在する場合のみ。 */
  sentimentSidecar?: Record<string, unknown>;
}

/** step2 出力 */
export type Step2Output = GameIntake;

// ── ③内部経済グラフ ──────────────────────────────────────────────────────────

export interface Resource {
  name: string;
  description?: string;
}

export interface ResourceEdge {
  sourceMechanic: string;
  targetResource: string;
  type: "produces" | "consumes";
}

export interface EconomyPhase0 {
  mechanics: MechanicMid[];
  resources: Resource[];
  edges: ResourceEdge[];
  /** true = economy-analyzer のキャッシュから取得 */
  cached: boolean;
}

/** step3 出力。data/restructure/<slug>/step3.json */
export type Step3Output = EconomyPhase0;

// ── ④評価 ────────────────────────────────────────────────────────────────────

export interface MechanicEvaluation {
  mechanicId: string;
  mechanicName: string;
  scale: "micro";
  score: number; // 0..1
  rationale: string;
  /** 嗜好プロファイルとの適合度 (cosine) */
  preferenceCompatibility: number;
  isNegative: boolean;
}

export interface MacroEvaluation {
  score: number;
  rationale: string;
  /** 内部経済の収支判定 */
  economyBalance: "balanced" | "inflationary" | "deflationary" | "deadend" | "unknown";
  /** true = LLM 推定 (実プレイデータなし)。spec §4-B */
  progressionCurveEstimated: true;
  progressionCurveSummary: string;
  mechanicInteractions: Array<{
    a: string;
    b: string;
    type: "synergy" | "cannibalize" | "isolated";
  }>;
}

export interface Step4Output {
  micro: MechanicEvaluation[];
  macro: MacroEvaluation;
  /** macro 評価のネガ + micro.isNegative の mechanic 名一覧 */
  negativeGapCandidates: string[];
}

// ── ⑤コア/オプション分解 ─────────────────────────────────────────────────────

export type MechanicRole = "core" | "option";

export interface MechanicRoleEntry {
  mechanicId: string;
  mechanicName: string;
  role: MechanicRole;
  rationale: string;
  /** resource グラフでの次数中心性スコア (0..1) */
  economyCentrality: number;
}

export interface Step5Output {
  roles: MechanicRoleEntry[];
  coreMechanics: string[];
  optionMechanics: string[];
}

// ── ⑥他ゲームのメカニクス結合 ────────────────────────────────────────────────

export interface CombinationCandidate {
  sourceGame: string;
  mechanic: MechanicMid;
  /** ④ 評価スコア (元ゲームでの値) */
  sourceScore: number;
  playContextCompatible: boolean;
  /** 嗜好プロファイルとの cosine */
  preferenceCompatibility: number;
  rationale: string;
  /** 出所透明性 (spec §4-D: provenance 必須) */
  provenance: string;
}

export interface Step6Output {
  candidates: CombinationCandidate[];
}

// ── ⑦破綻検出・修正 ─────────────────────────────────────────────────────────

export type IncoherenceType =
  | "economy_imbalance"
  | "progression_monotone"
  | "loop_dead_end"
  | "preference_mismatch"
  | "mechanic_conflict";

export interface IncoherenceIssue {
  type: IncoherenceType;
  description: string;
  affectedMechanics: string[];
  suggestedFix: string;
}

export interface Step7Output {
  issues: IncoherenceIssue[];
  iterationCount: number;
  /** 確定した修正後の hypothesis 案 */
  resolvedHypotheses: string[];
  converged: boolean;
}

// ── パイプライン全体 ──────────────────────────────────────────────────────────

export interface RestructurePipelineOutput {
  slug: string;
  step1: Step1Output;
  step2: Step2Output;
  step3: Step3Output;
  step4: Step4Output;
  step5: Step5Output;
  step6: Step6Output;
  step7: Step7Output;
}
