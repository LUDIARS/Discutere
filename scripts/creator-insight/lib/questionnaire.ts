/**
 * 人生脚本アンケート (`skills/creator-insight/data/life-script-questionnaire.ja.json`) の読み込みと形の検査。
 * creator-insight の集計ツール (life-script-items / score-life-script / plan-life-script-followups) が共有する。
 *
 * - ネットワークに出ない。LLM を呼ばない。標準ライブラリだけを使う。
 * - 入力ファイルは `MAX_INPUT_BYTES` を超えたら読まずにエラーにする (切り捨てない)。
 */
import { readFileSync, statSync } from "node:fs";

export const MAX_INPUT_BYTES = 256 * 1024;

export interface LikertItem { id: string; key: string; reverse: boolean; text: string }
export interface ForcedOption { id: string; key: string; text: string }
export interface ForcedItem { id: string; scene: string; options: ForcedOption[] }

export interface LikertSection { id: string; title: string; type: "likert"; items: LikertItem[] }
export interface ForcedSection { id: string; title: string; type: "forced_choice"; prompt: string; items: ForcedItem[] }
export type Section = LikertSection | ForcedSection;

export interface Scoring {
  scaleScore: { minAnsweredRatio: number; round: number };
  axes: { drivers: string[]; process: string[]; position: string[] };
  dominance: { minScore: number; minGapFromPersonMean: number; maxReported: number };
  forcedChoice: { agreeMinPicks: number };
  position: { midpoint: number; borderLow: number; borderHigh: number };
  responseStyle: {
    oneSidedHighMean: number; oneSidedLowMean: number; flatSdBelow: number; reverseConflictGap: number;
  };
  driverProcessCorrespondence: Record<string, string>;
}

export interface Questionnaire {
  id: string;
  revision: string;
  instructions: string[];
  scale: { labels: Record<string, string> };
  presentation: { shuffleWithinSection: boolean; seed: string };
  labels: Record<string, string>;
  sections: Section[];
  scoring: Scoring;
}

export class InputError extends Error {}

/** ファイルを上限つきで読み、JSON として解釈する。 */
export function readJsonFile(path: string): unknown {
  const size = statSync(path).size;
  if (size > MAX_INPUT_BYTES) throw new InputError(`input too large: ${path} (${size} bytes > ${MAX_INPUT_BYTES})`);
  try {
    return JSON.parse(readFileSync(path, "utf8"));
  } catch {
    throw new InputError(`not JSON: ${path}`);
  }
}

export function isRecord(v: unknown): v is Record<string, unknown> {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}

/** 設問 JSON の形を確かめる。ID の重複と、強制選択が 5 つのドライバーを 1 つずつ含むことも見る。 */
export function parseQuestionnaire(raw: unknown): Questionnaire {
  if (!isRecord(raw) || typeof raw.id !== "string" || typeof raw.revision !== "string") {
    throw new InputError("questionnaire: id / revision missing");
  }
  if (!Array.isArray(raw.sections) || !isRecord(raw.scoring)) throw new InputError("questionnaire: sections / scoring missing");
  const q = raw as unknown as Questionnaire;
  const seen = new Set<string>();
  const drivers = [...q.scoring.axes.drivers].sort().join(",");
  for (const s of q.sections) {
    if (!Array.isArray(s.items)) throw new InputError(`questionnaire: section ${s.id} has no items`);
    for (const item of s.items) {
      if (seen.has(item.id)) throw new InputError(`questionnaire: duplicate item id ${item.id}`);
      seen.add(item.id);
    }
    if (s.type === "forced_choice") {
      for (const item of s.items) {
        const keys = item.options.map((o) => o.key).sort().join(",");
        if (keys !== drivers) throw new InputError(`questionnaire: ${item.id} must contain each driver once`);
      }
    }
  }
  return q;
}

export function loadQuestionnaire(path: string): Questionnaire {
  return parseQuestionnaire(readJsonFile(path));
}

export function likertSections(q: Questionnaire): LikertSection[] {
  return q.sections.filter((s): s is LikertSection => s.type === "likert");
}

export function forcedSection(q: Questionnaire): ForcedSection | undefined {
  return q.sections.find((s): s is ForcedSection => s.type === "forced_choice");
}

/** 小数第 n 位で丸める。判定は丸める前の値で行い、丸めは出力にだけ使う。 */
export function round(v: number, digits: number): number {
  const f = 10 ** digits;
  return Math.round((v + Number.EPSILON) * f) / f;
}

/** CLI の失敗を 1 行の JSON で stderr に出して終了する (入力の内容は出さない)。 */
export function failCli(e: unknown): never {
  const message = e instanceof InputError ? e.message : "unexpected error";
  process.stderr.write(JSON.stringify({ error: message }) + "\n");
  process.exit(1);
}
