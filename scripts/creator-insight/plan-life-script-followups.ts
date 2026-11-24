/**
 * plan-life-script-followups — 人生脚本アンケートの採点結果から、追加質問の話題と最初の問いを決める。
 *
 * 入力: <採点結果 JSON (score-life-script の出力)> [追加質問 JSON] [設問 JSON]
 *   追加質問 JSON は省略時 skills/creator-insight/data/life-script-followups.ja.json。
 * 出力 (stdout, JSON): { followups: { questionnaire, revision, maxDeepeningPerTopic, topics: [{ n, kind, keys, ask, basis, strength? }] } }
 *   - 話題の選び方と順番はロジックで決める (同じ入力なら同じ出力)。問いの言い回しと深掘りは LLM がチャットで行う。
 *   - basis は話題を選んだ根拠 (採点結果のどの値か)。対話用メモの根拠 ID に使う。
 *   - 採点結果は変えない。追加質問の答えで得点・優位・確度を書き換えない。
 * 上限: 入力 256KB、話題は limits.maxTopics まで。ネットワークに出ない。LLM を呼ばない。
 *
 * 実行: npm run insight:plan-life-script-followups -- <採点結果 JSON> [追加質問 JSON] [設問 JSON]
 */
import path from "node:path";
import { fileURLToPath } from "node:url";
import { failCli, InputError, isRecord, loadQuestionnaire, readJsonFile, type Questionnaire } from "./lib/questionnaire.js";
import { DEFAULT_QUESTIONNAIRE } from "./life-script-items.js";
import type { LifeScriptResult } from "./score-life-script.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const DEFAULT_FOLLOWUPS = path.resolve(HERE, "../../skills/creator-insight/data/life-script-followups.ja.json");

export interface FollowupBank {
  id: string;
  revision: string;
  questionnaire: string;
  limits: { maxTopics: number; maxDeepeningPerTopic: number };
  drivers: Record<string, { ask: string; strength: string }>;
  process: Record<string, { story: string; ask: string }>;
  position: Record<string, string>;
  templates: Record<"forcedChoiceMismatch" | "correspondenceMismatch" | "reverseConflict" | "flat" | "oneSided" | "open", string>;
  directions: { high: string; low: string };
}

export type TopicKind =
  | "response_style" | "reverse_conflict" | "forced_choice_mismatch" | "dominant_driver"
  | "dominant_process" | "correspondence_mismatch" | "position" | "open";

export interface FollowupTopic {
  n: number;
  kind: TopicKind;
  keys: string[];
  ask: string;
  basis: string[];
  strength?: string;
}

export interface FollowupPlan {
  questionnaire: string;
  revision: string;
  maxDeepeningPerTopic: number;
  topics: FollowupTopic[];
}

export function parseBank(raw: unknown, q: Questionnaire): FollowupBank {
  if (!isRecord(raw) || raw.questionnaire !== q.id || !isRecord(raw.limits) || !isRecord(raw.templates)) {
    throw new InputError(`followups: not a bank for ${q.id}`);
  }
  const bank = raw as unknown as FollowupBank;
  for (const k of q.scoring.axes.drivers) if (!bank.drivers?.[k]) throw new InputError(`followups: driver ${k} missing`);
  for (const k of q.scoring.axes.process) if (!bank.process?.[k]) throw new InputError(`followups: process ${k} missing`);
  return bank;
}

export function parseResult(raw: unknown, q: Questionnaire): LifeScriptResult {
  const r = isRecord(raw) && isRecord(raw.lifeScript) ? raw.lifeScript : raw;
  if (!isRecord(r) || r.questionnaire !== q.id || r.revision !== q.revision || !isRecord(r.drivers) || !isRecord(r.process)) {
    throw new InputError(`result: not a ${q.id} ${q.revision} scoring result`);
  }
  return r as unknown as LifeScriptResult;
}

const fill = (template: string, vars: Record<string, string>): string =>
  template.replace(/\{(\w+)\}/g, (m, k: string) => vars[k] ?? m);

/** 話題の候補を優先順に並べる。答え方の偏り → 逆転の矛盾 → 強制選択との食い違い → 優位 → 対応の食い違い → 構え。 */
function candidates(r: LifeScriptResult, bank: FollowupBank, q: Questionnaire): Omit<FollowupTopic, "n">[] {
  const label = (k: string): string => q.labels[k] ?? k;
  const t = bank.templates;
  const out: Omit<FollowupTopic, "n">[] = [];
  const rs = r.responseStyle;

  if (rs.flat) out.push({ kind: "response_style", keys: [], ask: t.flat, basis: [`responseStyle.rawSd=${rs.rawSd}`] });
  else if (rs.oneSided) {
    const direction = (rs.rawMean ?? 0) >= 2.5 ? bank.directions.high : bank.directions.low;
    out.push({ kind: "response_style", keys: [], ask: fill(t.oneSided, { direction }), basis: [`responseStyle.rawMean=${rs.rawMean}`] });
  }

  const conflictKeys = [...new Set(rs.reverseConflicts.map((c) => c.key))];
  for (const key of conflictKeys) {
    const items = rs.reverseConflicts.filter((c) => c.key === key).map((c) => c.item);
    out.push({ kind: "reverse_conflict", keys: [key], ask: fill(t.reverseConflict, { label: label(key) }), basis: items.map((i) => `responseStyle.reverseConflicts.${i}`) });
  }

  const agree = q.scoring.forcedChoice.agreeMinPicks;
  for (const key of r.drivers.dominant) {
    const picks = r.drivers.forcedChoice[key] ?? 0;
    if (picks < agree) {
      out.push({ kind: "forced_choice_mismatch", keys: [key], ask: fill(t.forcedChoiceMismatch, { label: label(key) }), basis: [`drivers.scores.${key}=${r.drivers.scores[key]}`, `drivers.forcedChoice.${key}=${picks}`] });
    }
  }

  for (const key of r.drivers.dominant) {
    const d = bank.drivers[key]!;
    out.push({ kind: "dominant_driver", keys: [key], ask: d.ask, basis: [`drivers.scores.${key}=${r.drivers.scores[key]}`, `drivers.confidence.${key}=${r.drivers.confidence[key]}`], strength: d.strength });
  }
  for (const key of r.process.dominant) {
    out.push({ kind: "dominant_process", keys: [key], ask: bank.process[key]!.ask, basis: [`process.scores.${key}=${r.process.scores[key]}`, `process.confidence.${key}=${r.process.confidence[key]}`] });
  }

  for (const [driver, proc] of Object.entries(q.scoring.driverProcessCorrespondence)) {
    if (r.drivers.dominant.includes(driver) && !r.process.dominant.includes(proc)) {
      out.push({
        kind: "correspondence_mismatch", keys: [driver, proc],
        ask: fill(t.correspondenceMismatch, { driver: label(driver), process: label(proc), story: bank.process[proc]!.story }),
        basis: [`drivers.dominant=${driver}`, `process.scores.${proc}=${r.process.scores[proc]}`],
      });
    }
  }

  const p = r.position;
  if (p.selfBand && p.otherBand) {
    const quadrant = p.selfBand === "border" || p.otherBand === "border" ? "border" : `${p.selfBand}/${p.otherBand}`;
    const ask = bank.position[quadrant];
    if (ask) out.push({ kind: "position", keys: ["self", "other"], ask, basis: [`position.self=${p.self}`, `position.other=${p.other}`] });
  }
  return out;
}

export function planFollowups(r: LifeScriptResult, bank: FollowupBank, q: Questionnaire): FollowupPlan {
  const picked = candidates(r, bank, q).slice(0, bank.limits.maxTopics);
  if (picked.length === 0) picked.push({ kind: "open", keys: [], ask: bank.templates.open, basis: ["no-dominant"] });
  return {
    questionnaire: q.id,
    revision: q.revision,
    maxDeepeningPerTopic: bank.limits.maxDeepeningPerTopic,
    topics: picked.map((topic, i) => ({ n: i + 1, ...topic })),
  };
}

function main(): void {
  const [resultPath, bankPath, questionnairePath] = process.argv.slice(2);
  if (!resultPath) throw new InputError("usage: plan-life-script-followups <result.json> [followups.json] [questionnaire.json]");
  const q = loadQuestionnaire(questionnairePath ?? DEFAULT_QUESTIONNAIRE);
  const bank = parseBank(readJsonFile(bankPath ?? DEFAULT_FOLLOWUPS), q);
  const plan = planFollowups(parseResult(readJsonFile(resultPath), q), bank, q);
  process.stdout.write(JSON.stringify({ followups: plan }, null, 2) + "\n");
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try { main(); } catch (e) { failCli(e); }
}
