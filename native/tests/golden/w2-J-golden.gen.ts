// w2-J (診断の契約・Gap率・要約・クエリ正規化・KG レジストリ・backup key) の Node 実装の入出力を記録する。
// 実行: npx tsx native/tests/golden/w2-J-golden.gen.ts
// 出力: native/tests/fixtures/w2j/*.json (値はすべて架空)
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import Database from "better-sqlite3";

import { validateLevelCheckRequest } from "../../../src/level-check/request.js";
import { buildLevelCheckPrompt } from "../../../src/level-check/prompt.js";
import { parseLevelCheckResponse } from "../../../src/level-check/response.js";
import { calculatePlayEnvelopes } from "../../../src/level-check/play-envelope.js";
import { composePlayFindings } from "../../../src/level-check/play-findings.js";
import { validateMechanicsCheckRequest } from "../../../src/mechanics-check/request.js";
import { buildMechanicsCheckPrompt } from "../../../src/mechanics-check/prompt.js";
import { parseMechanicsCheckResponse } from "../../../src/mechanics-check/response.js";
import { analyzeGapRate, formatGapReport } from "../../../src/analysis/gap-rate.js";
import { createLlmSummarizer } from "../../../src/crawler/sources/summarize.js";
import { normalizeQueryText } from "../../../src/core/vectors/query-embed-cache.js";
import { applyMigrations } from "../../../src/core/db/schema.js";
import { requestFixture as levelRequest, differenceFixture as levelDiff } from "../../../tests/level-check/fixtures.js";
import { requestFixture as mechRequest, differenceFixture as mechDiff } from "../../../tests/mechanics-check/fixtures.js";

const root = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "w2j");
fs.mkdirSync(root, { recursive: true });
const write = (name: string, value: unknown) => {
  fs.writeFileSync(path.join(root, name), JSON.stringify(value, null, 2) + "\n");
  console.log(`wrote ${name}`);
};
const run = <T>(fn: () => T) => {
  try {
    return { ok: true, value: JSON.parse(JSON.stringify(fn())) };
  } catch (e) {
    return { ok: false, code: (e as { code?: string }).code ?? null, error: (e as Error).message };
  }
};
const clone = <T>(v: T): T => JSON.parse(JSON.stringify(v));

// ── level-check ──
type LR = ReturnType<typeof levelRequest>;
const levelMutations: Array<[string, (r: LR) => unknown]> = [
  ["base", (r) => r],
  ["blank", () => ({ levelText: " " })],
  ["too-long", () => ({ levelText: "x".repeat(20_001) })],
  ["exact-limit", () => ({ levelText: "x".repeat(20_000) })],
  ["extra-field", () => ({ levelText: "x", fetch: "https://example.com" })],
  ["reserved-ref", () => ({ levelText: "x", references: [{ id: "level", source: "x", text: "x", evidenceKind: "observation" }] })],
  ["too-many-refs", () => ({ levelText: "x", references: Array.from({ length: 17 }, (_, i) => ({ id: `r${i}`, source: "x", text: "x", evidenceKind: "specification" })) })],
  ["total-chars", () => ({ levelText: "x".repeat(20_000), baselineText: "x".repeat(20_000), references: Array.from({ length: 4 }, (_, i) => ({ id: `r${i}`, source: "x", text: "x".repeat(8_000), evidenceKind: "specification" })) })],
  ["total-chars-exact", () => ({ levelText: "x".repeat(20_000), baselineText: "x".repeat(20_000), references: Array.from({ length: 3 }, (_, i) => ({ id: `r${i}`, source: "x", text: "x".repeat(8_000), evidenceKind: "specification" })) })],
  ["dup-ref", (r) => { r.references!.push({ ...r.references![0] }); return r; }],
  ["dup-event", (r) => { r.playModel!.events.push({ ...r.playModel!.events[0] }); return r; }],
  ["unknown-evidence", (r) => { r.playModel!.events[0].evidenceRefs = ["not-supplied"]; return r; }],
  ["dup-evidence", (r) => { r.playModel!.events[0].evidenceRefs = ["level", "level"]; return r; }],
  ["missing-event", (r) => { r.playModel!.scenarios[0].outcomes[0].eventIds = ["missing-event"]; return r; }],
  ["dup-event-ids", (r) => { r.playModel!.scenarios[0].outcomes[0].eventIds = ["bonus", "bonus"]; return r; }],
  ["unit", (r) => { r.playModel!.scenarios[0].outcomes[0].unit = "seconds"; return r; }],
  ["baseline-unit", (r) => { r.playModel!.scenarios[0].predictedBaseline!.unit = "seconds"; return r; }],
  ["prob-neg", (r) => { r.playModel!.scenarios[0].outcomes[0].probability = -0.1; return r; }],
  ["prob-big", (r) => { r.playModel!.scenarios[0].outcomes[0].probability = 1.1; return r; }],
  ["prob-sum-low", (r) => { r.playModel!.scenarios[0].outcomes[0].probability = 0.1; return r; }],
  ["prob-sum-high", (r) => { r.playModel!.scenarios[0].outcomes[0].probability = 0.6; return r; }],
  ["dup-scenario", (r) => { r.playModel!.scenarios.push(clone(r.playModel!.scenarios[0])); return r; }],
  ["dup-outcome", (r) => { r.playModel!.scenarios[0].outcomes[1].id = "low"; return r; }],
  ["dup-combo", (r) => { r.playModel!.scenarios[0].outcomes[1].eventIds = []; return r; }],
  ["combo-order", (r) => { r.playModel!.scenarios[0].outcomes[2].eventIds = ["bonus", "success"]; return r; }],
  ["trial-frac", (r) => { r.playModel!.scenarios[0].assumptions.trialCount = 1.5; return r; }],
  ["time-zero", (r) => { r.playModel!.scenarios[0].assumptions.timeLimitSeconds = 0; return r; }],
  ["time-ok", (r) => { r.playModel!.scenarios[0].assumptions.timeLimitSeconds = 12.5; return r; }],
  ["other-9", (r) => { r.playModel!.scenarios[0].assumptions.other = Array(9).fill("condition"); return r; }],
  ["other-8", (r) => { r.playModel!.scenarios[0].assumptions.other = Array(8).fill("condition"); return r; }],
  ["exhaustive-str", (r) => { (r.playModel!.scenarios[0] as any).exhaustive = "yes"; return r; }],
  ["partial", (r) => { delete r.playModel!.scenarios[0].outcomes[1].probability; return r; }],
  ["non-exhaustive", (r) => { r.playModel!.scenarios[0].exhaustive = false; return r; }],
  ["missing", (r) => { const s = r.playModel!.scenarios[0]; delete s.outcomes[0].probability; delete s.predictedBaseline; delete s.skillBand; s.assumptions = {}; return r; }],
  ["zero-prob", (r) => { r.playModel!.scenarios[0].outcomes[0].probability = 0; r.playModel!.scenarios[0].outcomes[1].probability = 0.75; return r; }],
  ["lower", (r) => { r.playModel!.metric.direction = "lower"; r.playModel!.metric.unit = "seconds"; const s = r.playModel!.scenarios[0]; s.predictedBaseline = { value: 8, unit: "seconds" }; s.outcomes.forEach((o, i) => { o.value = [4, 8, 12][i]; o.unit = "seconds"; }); return r; }],
  ["overflow", (r) => { r.playModel!.scenarios[0].outcomes[0].value = -1e308; r.playModel!.scenarios[0].outcomes[2].value = 1e308; return r; }],
  ["no-model", (r) => { delete r.playModel; return r; }],
  ["null-revision", (r) => { (r.references![0] as any).revision = null; return r; }],
  ["bad-id", (r) => { r.playModel!.events[0].id = "-bad"; return r; }],
];
const level = levelMutations.map(([name, mutate]) => {
  const input = mutate(levelRequest());
  const validated = run(() => validateLevelCheckRequest(input));
  const out: Record<string, unknown> = { name, input, validated };
  if (validated.ok) {
    const req = validateLevelCheckRequest(input);
    out.prompt = run(() => buildLevelCheckPrompt(req));
    out.envelopes = run(() => req.playModel ? calculatePlayEnvelopes(req.playModel) : []);
    out.findings = run(() => composePlayFindings(req, req.playModel ? calculatePlayEnvelopes(req.playModel) : []));
  }
  return out;
});
write("level_requests.json", level);

const responses = (diff: () => any, primaryId: string, kinds: string[]): Array<{ name: string; text: string; extraRef?: unknown }> => {
  const d = diff;
  const list: Array<{ name: string; text: string; extraRef?: unknown }> = [];
  const add = (name: string, value: unknown, extraRef?: unknown) => list.push({ name, text: typeof value === "string" ? value : JSON.stringify(value), extraRef });
  add("valid", { logic_differences: [d()], design_gap: [] });
  add("not-json", "not JSON");
  add("fenced", "```json\n" + JSON.stringify({ logic_differences: [d()], design_gap: [] }) + "\n```");
  add("empty", { logic_differences: [], design_gap: [] });
  add("extra", { logic_differences: [], design_gap: [], summary: "ok" });
  add("too-big", "x".repeat(131_073));
  const h1 = d(); h1.right.evidence[0].referenceId = "not-provided"; add("unknown-ref", { logic_differences: [h1], design_gap: [] });
  const h2 = d(); h2.right.evidence[0] = { referenceId: primaryId, quote: "実プレイで面白さが証明された。" }; add("bad-quote", { logic_differences: [h2], design_gap: [] });
  add("play-envelope", { logic_differences: [{ ...d(), kind: "play_envelope", envelope: { expectedValue: 999 } }], design_gap: [] });
  const u = d(); u.right.evidence = []; u.status = "consistent"; add("unsupported", { logic_differences: [u], design_gap: [] });
  add("contradiction-gap", { logic_differences: [], design_gap: [{ ...d(), kind: "contradiction" }] });
  add("linked-gap", { logic_differences: [{ ...d(), kind: kinds[0] }], design_gap: [] });
  const both = d(); both.causeDomain = "both"; add("both-insufficient", { logic_differences: [both], design_gap: [] });
  const both2 = d(); both2.causeDomain = "both"; both2.right.evidence.push({ referenceId: "layout", quote: "配置は一本道。" });
  add("both-grounded", { logic_differences: [both2], design_gap: [] }, { id: "layout", source: "supplied placement", text: "配置は一本道。", evidenceKind: "specification", domain: primaryId === "spec" ? "level" : "mechanics" });
  add("wrong-kind", { logic_differences: [{ ...d(), kind: "nope" }], design_gap: [] });
  const g = d(); g.status = "consistent"; add("gap-known", { logic_differences: [], design_gap: [{ ...g, kind: "missing_evidence" }] });
  const unk = d(); unk.status = "unknown"; unk.unknowns = []; add("unknown-no-reason", { logic_differences: [unk], design_gap: [] });
  const nc = d(); nc.conditions = []; nc.causeDomain = "unknown"; add("no-conditions", { logic_differences: [nc], design_gap: [] });
  const hyp = d(); add("causal-hypothesis", { logic_differences: [hyp], design_gap: [] }, { id: "hyp", source: "h", text: "仮説", evidenceKind: "causal_hypothesis" });
  const full = d(); full.right.evidence = []; full.unknowns = Array(16).fill("u"); full.status = "consistent"; add("no-room", { logic_differences: [full], design_gap: [] });
  add("deep", "[".repeat(40) + "]".repeat(40));
  add("missing-target", { logic_differences: [(() => { const x = d(); delete x.target; return x; })()], design_gap: [] });
  return list;
};
const levelResponses = responses(levelDiff, "level", ["scenario_condition"]).map((c) => {
  const req = levelRequest();
  if (c.extraRef) req.references!.push(c.extraRef as any);
  const validated = validateLevelCheckRequest(req);
  return { ...c, request: req, result: run(() => parseLevelCheckResponse(c.text, validated)) };
});
write("level_responses.json", levelResponses);

const mechInputs: Array<[string, () => unknown]> = [
  ["base", () => mechRequest()],
  ["blank", () => ({ specText: " " })],
  ["model", () => ({ specText: "spec", playModel: {} })],
  ["domain-both", () => ({ specText: "x", references: [{ id: "a", source: "x", text: "x", evidenceKind: "specification", domain: "both" }] })],
  ["array", () => []],
  ["no-refs", () => ({ specText: "spec" })],
];
write("mechanics_requests.json", mechInputs.map(([name, f]) => {
  const input = f();
  const validated = run(() => validateMechanicsCheckRequest(input));
  return { name, input, validated, prompt: validated.ok ? run(() => buildMechanicsCheckPrompt(validateMechanicsCheckRequest(input))) : null };
}));
write("mechanics_responses.json", responses(mechDiff, "spec", ["mda_causality"]).map((c) => {
  const req = mechRequest();
  if (c.extraRef) req.references!.push(c.extraRef as any);
  return { ...c, request: req, result: run(() => parseMechanicsCheckResponse(c.text, validateMechanicsCheckRequest(req))) };
}));

// ── Gap率 (架空のゲーム定義と発話) ──
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "w2j-golden-"));
const gamesDir = path.join(tmp, "games");
fs.mkdirSync(gamesDir);
const md = "---\nid: fic-game\ntitle: Fictional Game\nmechanics:\n  - name: 収集要素の長い名前がここにあって二十二文字を超えるもの\n    intended_valence: positive\n    intended_aspects: [reward, progression]\n  - name: 協力\n    intended_valence: positive\n    intended_aspects: [social]\n  - name: 難所\n    intended_valence: negative\n    intended_aspects: [difficulty]\n  - name: 未定義\n---\n";
fs.writeFileSync(path.join(gamesDir, "fic-game.md"), md);
const texts = ["最高に楽しい、報酬が嬉しい", "つまらない、ひどいバランス", "難しいけど達成感がある", "友達と協力できて楽しい", "普通", "ガチャ課金がひどい", "グラフィックが綺麗で感動", "ストーリーが退屈", "操作が難しすぎてイライラ", "レベル上げが楽しい"];
const sessions = [
  { id: "s1", title: "steam:fic-game" }, { id: "s2", title: "youtube:fic-game" }, { id: "s3", title: ":fic-game" }, { id: "s4", title: "steam:other" },
];
const utterances = texts.map((t, i) => ({ id: `u${i}`, session_id: sessions[i % 4].id, raw_content: t, posted_at: i === 4 ? 0 : Date.UTC(2026, i % 3, 5 + i) }));
const dbPath = path.join(tmp, "kg.sqlite");
const db = new Database(dbPath);
applyMigrations(db);
const at = 1;
for (const s of sessions) db.prepare("INSERT INTO sessions (id, workspace_id, title, started_at, created_at, updated_at) VALUES (?, 'ws', ?, ?, ?, ?)").run(s.id, s.title, at, at, at);
for (const u of utterances) db.prepare("INSERT INTO utterances (id, workspace_id, session_id, raw_content, posted_at, created_at, updated_at) VALUES (?, 'ws', ?, ?, ?, ?, ?)").run(u.id, u.session_id, u.raw_content, u.posted_at, at, at);
db.close();
const report = analyzeGapRate("fic-game", gamesDir, dbPath);
write("gap_rate.json", { md, sessions, utterances, report, text: formatGapReport(report), missing: run(() => analyzeGapRate("nope", gamesDir, dbPath)) });

// ── 要約 ──
const summaries: unknown[] = [];
for (const [target, reply, input] of [[400, "短い要約", "本文"], [3, "あいうえおかきくけこ", "x"], [6.5, "𠮷𠮷𠮷𠮷𠮷𠮷𠮷𠮷𠮷𠮷𠮷", "長い入力".repeat(5000)], [400, "  ", "本文"], [400, "x", "   "]] as Array<[number, string, string]>) {
  let seen: any = null;
  const s = createLlmSummarizer({ invoke: async (a: any) => { seen = a; return { ok: true, text: reply }; } } as any, { targetChars: target, inputCap: 7 });
  const out = await s.summarize(input);
  summaries.push({ target, reply, input, out, prompt: seen?.prompt ?? null, system: seen?.system ?? null });
}
write("summarize.json", summaries);

// ── クエリ正規化 ──
const terms = [[" Foo ", "bar", "foo", "", "Ｂａｒ", "あいう", "Zeta", "alpha", "𠮷", "ｱ"], ["b", "a", "B"], []];
write("normalize_query.json", terms.map((t) => ({ terms: t, out: normalizeQueryText(t) })));
fs.rmSync(tmp, { recursive: true, force: true });
