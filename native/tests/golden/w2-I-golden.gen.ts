// w2-I 担当分 (persona-engine 本体・常駐ワーカープールの API・外部会話・使い方ガイド) の Node 実装の入出力を記録する。
// 実行: npx tsx native/tests/golden/w2-I-golden.gen.ts
// 出力:
//   native/web/api/{guide.html, worker-pool-control.html}  … 画面 (Node が返す文字列。guide は {{名前}} の印入り)
//   native/tests/fixtures/w2i/*.json                        … C++ テストが同じ出力を返すかを確かめる記録
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import Database from "better-sqlite3";

const here = path.dirname(new URL(import.meta.url).pathname);
const nativeDir = path.join(here, "..", "..");
const fixturesDir = path.join(nativeDir, "tests", "fixtures", "w2i");
fs.mkdirSync(fixturesDir, { recursive: true });
const writeFixture = (name: string, value: unknown) => {
  fs.writeFileSync(path.join(fixturesDir, name), JSON.stringify(value, null, 2) + "\n");
  console.log(`wrote ${name}`);
};

const { handleAction, __test, _checkForbiddenRule } = await import("../../../src/persona-engine/engine/handler.js");
const { applyPersonaEngineMigrations } = await import("../../../src/persona-engine/db/migrations.js");
const { PersonasRepo } = await import("../../../src/persona-engine/db/personas-repo.js");
const { RulesRepo } = await import("../../../src/persona-engine/db/rules-repo.js");
const { DISCUSSION_PERSONA_SEEDS } = await import("../../../src/persona-engine/seeds/personas.js");
const { DISCUSSION_RULE_SEEDS } = await import("../../../src/persona-engine/seeds/rules.js");
const { recordHypothesisOutcome } = await import("../../../src/persona-engine/learning/learner.js");
const { personaMetrics, ruleMetrics } = await import("../../../src/persona-engine/learning/metrics.js");
const { createGuideRoutes } = await import("../../../src/api/guide-routes.js");
const { workerPoolControlRoutes } = await import("../../../src/api/worker-pool-control.js");
const { ExternalDiscussionService } = await import("../../../src/external-discussion/service.js");
const { ParticipationStore } = await import("../../../src/external-discussion/store.js");
const { parseConversation } = await import("../../../src/external-discussion/contracts.js");

// ─── extractJson ──────────────────────────────────────────────────────────
const extractInputs = [
  '{"action":"skip"}',
  '  {"action":"skip","reasoning":"x"}  ',
  'Sure! here it is: {"action":"post_utterance","text":"hi"} thanks',
  "```json\n{\"action\":\"propose_hypothesis\",\"statement\":\"s\"}\n```",
  '[1,2,3]',
  '"just a string"',
  "no json at all",
  "{not json} and then {\"action\":\"skip\"}",
  "{\"a\":{\"b\":{\"c\":1}}} trailing",
  "{\"a\":\"}\"} after",
  "{{{",
  "}}}{\"action\":\"skip\"}",
  "{\"x\":1}{\"y\":2}",
  "前置き {\"action\":\"post_utterance\",\"text\":\"日本語の発話\"} 後書き",
  "",
  "   ",
  "null",
  "{\"action\":\"skip\"",
];
writeFixture("extract_json.json", extractInputs.map((input) => ({ input, output: __test.extractJson(input) })));

// ─── checkForbiddenRule ───────────────────────────────────────────────────
const forbiddenInputs: Array<{ id: string; description: string | null; instructions: string }> = [
  { id: "ok-rule", description: null, instructions: "議論の論点を整理して提案する" },
  { id: "silent-check", description: null, instructions: "沈黙確認用" },
  { id: "x", description: "無音の時に動く", instructions: "abc" },
  { id: "x", description: null, instructions: "SILENCE detector" },
  { id: "x", description: null, instructions: "Quiet-Check every minute" },
  { id: "x", description: null, instructions: "quiet_check" },
  { id: "x", description: null, instructions: "quiet check" },
  { id: "x", description: null, instructions: "quiet  check" },
  { id: "x", description: null, instructions: "quietcheck" },
  { id: "x", description: null, instructions: "進捗どうですか" },
  { id: "x", description: null, instructions: "進捗を聞く" },
  { id: "x", description: null, instructions: "進捗PING" },
  { id: "x", description: null, instructions: "進捗報告" },
  { id: "x", description: null, instructions: "progress-ping" },
  { id: "x", description: null, instructions: "Progress Check" },
  { id: "x", description: null, instructions: "progress__check" },
  { id: "x", description: null, instructions: "汎用雑談をする" },
  { id: "x", description: null, instructions: "general_chitchat" },
  { id: "x", description: null, instructions: "small-talk-random" },
  { id: "x", description: null, instructions: "smalltalkrandom" },
  { id: "x", description: null, instructions: "small talk" },
  { id: "x", description: null, instructions: "全 persona を起こす" },
  { id: "x", description: null, instructions: "全　　Persona" },
  { id: "x", description: null, instructions: "全\npersona" },
  { id: "x", description: null, instructions: "全員 persona" },
  { id: "x", description: null, instructions: "all-personas-fire" },
  { id: "x", description: null, instructions: "all personas act" },
  { id: "x", description: null, instructions: "allpersonainvoke" },
  { id: "x", description: null, instructions: "all-personassfire" },
  { id: "x", description: null, instructions: "ALL_PERSONA_ACTION" },
  { id: "all-personas", description: null, instructions: "fire" },
  { id: "x", description: "progress", instructions: "check" },
  { id: "x", description: null, instructions: "ſilence" },
];
writeFixture(
  "forbidden.json",
  forbiddenInputs.map((input) => ({ input, output: _checkForbiddenRule(input) }))
);

// ─── handleAction ─────────────────────────────────────────────────────────
type HandlerCase = {
  name: string;
  rawText: string;
  ruleId?: string;
  personaId?: string;
  sessionId?: string | null;
  triggerEvent?: { kind: string; payload?: unknown };
  findSession?: string | null;
  findActive?: string | null;
  /** remove_rule の正常系用: 先に AI 追加 rule を入れておく */
  preAddRule?: string;
};
const long = "これは十分に長い指示文で、議論の論点を一つ選び具体的な提案をする。";
const handlerCases: HandlerCase[] = [
  { name: "invalid json", rawText: "not a json {{" },
  { name: "invalid json long", rawText: "x".repeat(100) + "日本語".repeat(40) },
  { name: "skip", rawText: '{"action":"skip","reasoning":"nothing"}' },
  { name: "skip no reason", rawText: '{"action":"skip"}' },
  { name: "propose", rawText: '{"action":"propose_hypothesis","statement":"案A","addresses_gap_id":"g-1","reasoning":"r"}' },
  { name: "propose gap_id", rawText: '{"action":"propose_hypothesis","statement":"案B","gap_id":"g-2"}' },
  { name: "propose empty addresses", rawText: '{"action":"propose_hypothesis","statement":"案","addresses_gap_id":""}' },
  { name: "propose infer", rawText: '{"action":"propose_hypothesis","statement":"案C"}', triggerEvent: { kind: "DesignGapDetected", payload: { id: "g-9" } } },
  { name: "propose infer other kind", rawText: '{"action":"propose_hypothesis","statement":"案C"}', triggerEvent: { kind: "Other", payload: { id: "g-9" } } },
  { name: "propose infer empty id", rawText: '{"action":"propose_hypothesis","statement":"案C"}', triggerEvent: { kind: "DesignGapUpdated", payload: { id: "" } } },
  { name: "propose blank", rawText: '{"action":"propose_hypothesis","statement":"   "}' },
  { name: "propose non-string", rawText: '{"action":"propose_hypothesis","statement":5}' },
  { name: "post with session", rawText: '{"action":"post_utterance","text":"発話","responds_to":"u-0"}', sessionId: "s-1" },
  { name: "post alias say", rawText: '{"action":"say","text":"言う"}', sessionId: "s-1" },
  { name: "post alias without text", rawText: '{"action":"talk"}', sessionId: "s-1" },
  { name: "post by hypothesis", rawText: '{"action":"post_utterance","text":"t","hypothesis_id":"h-1"}', findSession: "s-h", findActive: "s-a" },
  { name: "post hypothesis miss", rawText: '{"action":"post_utterance","text":"t","hypothesis_id":"h-1"}', findSession: null, findActive: "s-a" },
  { name: "post active", rawText: '{"action":"post_utterance","text":"t"}', findActive: "s-a" },
  { name: "post none", rawText: '{"action":"post_utterance","text":"t"}' },
  { name: "post empty session", rawText: '{"action":"post_utterance","text":"t"}', sessionId: "", findActive: "s-a" },
  { name: "post blank", rawText: '{"action":"post_utterance","text":"  "}', sessionId: "s-1" },
  { name: "unknown", rawText: '{"action":"dance"}' },
  { name: "no action", rawText: '{"text":"x"}' },
  { name: "add_rule ok", rawText: JSON.stringify({ action: "add_rule", rule: { id: "extra-counter", description: "テスト", trigger_type: "tick", tick_sec: 600, target: "advocate", cooldown_sec: 300, instructions: long } }) },
  { name: "add_rule event defaults", rawText: JSON.stringify({ action: "add_rule", rule: { id: "  ev-rule  ", trigger_type: "event", tick_sec: 2, cooldown_sec: -1, event_kind: "GapDetected", instructions: long } }) },
  { name: "add_rule no rule", rawText: '{"action":"add_rule"}' },
  { name: "add_rule array", rawText: '{"action":"add_rule","rule":[1]}' },
  { name: "add_rule bad id", rawText: JSON.stringify({ action: "add_rule", rule: { id: "Bad_Id", trigger_type: "tick", instructions: long } }) },
  { name: "add_rule one char id", rawText: JSON.stringify({ action: "add_rule", rule: { id: "a", trigger_type: "tick", instructions: long } }) },
  { name: "add_rule 64 id", rawText: JSON.stringify({ action: "add_rule", rule: { id: "a" + "b".repeat(63), trigger_type: "tick", instructions: long } }) },
  { name: "add_rule 65 id", rawText: JSON.stringify({ action: "add_rule", rule: { id: "a" + "b".repeat(64), trigger_type: "tick", instructions: long } }) },
  { name: "add_rule bad trigger", rawText: JSON.stringify({ action: "add_rule", rule: { id: "abc", trigger_type: "cron", instructions: long } }) },
  { name: "add_rule short", rawText: JSON.stringify({ action: "add_rule", rule: { id: "abc", trigger_type: "tick", instructions: "短い" } }) },
  { name: "add_rule 16", rawText: JSON.stringify({ action: "add_rule", rule: { id: "abc", trigger_type: "tick", instructions: "あ".repeat(16) } }) },
  { name: "add_rule 4000", rawText: JSON.stringify({ action: "add_rule", rule: { id: "abd", trigger_type: "tick", instructions: "x".repeat(4000) } }) },
  { name: "add_rule 4001", rawText: JSON.stringify({ action: "add_rule", rule: { id: "abe", trigger_type: "tick", instructions: "x".repeat(4001) } }) },
  { name: "add_rule forbidden", rawText: JSON.stringify({ action: "add_rule", rule: { id: "silent-check", trigger_type: "tick", instructions: "沈黙確認用の rule、これは禁則パターンに一致する" } }) },
  { name: "add_rule duplicate", rawText: JSON.stringify({ action: "add_rule", rule: { id: "propose-on-gap", trigger_type: "tick", instructions: long } }) },
  { name: "remove seed", rawText: '{"action":"remove_rule","rule_id":"propose-on-gap","reasoning":"r"}' },
  { name: "remove unknown", rawText: '{"action":"remove_rule","rule_id":"nope"}' },
  { name: "remove missing", rawText: '{"action":"remove_rule"}' },
  { name: "remove ok", rawText: '{"action":"remove_rule","rule_id":"extra-x","reasoning":"不要"}', preAddRule: "extra-x" },
];

const handlerResults = handlerCases.map((hc) => {
  const db = new Database(":memory:");
  applyPersonaEngineMigrations(db);
  new PersonasRepo(db).bulkSeed(DISCUSSION_PERSONA_SEEDS);
  const rules = new RulesRepo(db);
  rules.bulkSeed(DISCUSSION_RULE_SEEDS);
  if (hc.preAddRule) rules.insertOrIgnore({ id: hc.preAddRule, trigger_type: "tick", tick_sec: 60, instructions: long }, { addedBy: "ai:x" });
  const firstLogId = (db.prepare("SELECT COALESCE(MAX(id),0) AS m FROM rule_log").get() as { m: number }).m;
  const loggerCalls: unknown[] = [];
  const proposed: unknown[] = [];
  const posted: unknown[] = [];
  const findCalls: unknown[] = [];
  const ctx: Record<string, unknown> = {
    listActiveHypotheses: () => [],
    listRecentGaps: () => [],
    listRecentUtterances: () => [],
    proposeHypothesis: (input: unknown) => { proposed.push(input); return { id: `h-${proposed.length}` }; },
    postUtterance: (input: unknown) => { posted.push(input); return { id: `u-${posted.length}` }; },
  };
  if (hc.findSession !== undefined) ctx.findDiscussionSession = (q: unknown) => { findCalls.push({ fn: "find", q }); return hc.findSession; };
  if (hc.findActive !== undefined) ctx.findActiveDiscussionSession = (q: unknown) => { findCalls.push({ fn: "active", q }); return hc.findActive; };
  const logger = {
    debug: () => {},
    info: (meta: unknown, msg: string) => loggerCalls.push({ level: "info", meta, msg }),
    warn: (meta: unknown, msg: string) => loggerCalls.push({ level: "warn", meta, msg }),
    error: (meta: unknown, msg: string) => loggerCalls.push({ level: "error", meta, msg }),
  };
  const result = handleAction({
    ruleId: hc.ruleId ?? "meta-improve-rules",
    personaId: hc.personaId ?? "neutral-observer",
    workspaceId: "knowledge",
    sessionId: hc.sessionId === undefined ? null : hc.sessionId,
    triggerEvent: hc.triggerEvent,
    rawText: hc.rawText,
    contextProvider: ctx as never,
    rules,
    logger,
  });
  const logs = db.prepare("SELECT rule_id, action, actor, detail FROM rule_log WHERE id > ? ORDER BY id").all(firstLogId);
  const added = db.prepare("SELECT id, description, trigger_type, tick_sec, event_kind, target, cooldown_sec, instructions, added_by, enabled, removed_by, removed_reason FROM rules WHERE id NOT IN (" + DISCUSSION_RULE_SEEDS.map(() => "?").join(",") + ") ORDER BY id").all(...DISCUSSION_RULE_SEEDS.map((r) => r.id));
  db.close();
  return { case: hc, result, logs, loggerCalls, proposed, posted, findCalls, rulesAfter: added };
});
writeFixture("handle_action.json", handlerResults);

// ─── learner / metrics ────────────────────────────────────────────────────
// 正規表現の一致そのもの (learner.ts / metrics.ts の PROPOSE_PATTERN と同じ式)。
const PROPOSE_PATTERN = /persona\s+(\S+)\s+proposed\s+hypothesis\s+(\S+)/i;
const details = [
  "persona advocate proposed hypothesis h-001",
  "PERSONA Advocate Proposed Hypothesis H-1",
  "persona  sceptic\tproposed\nhypothesis   h-002 extra",
  "xx persona a proposed hypothesis b",
  "persona proposed hypothesis h",
  "personapersona a proposed hypothesis b",
  "persona a proposed hypothesis",
  "persona a  proposed  hypothesis　日本語ID 後ろ",
  "persona advocate posted utterance u-1",
  "persona a proposed hypothesis b; persona c proposed hypothesis d",
  "",
];
writeFixture(
  "propose_pattern.json",
  details.map((d) => {
    const m = PROPOSE_PATTERN.exec(d);
    return { detail: d, match: m ? { personaId: m[1], hypothesisId: m[2] } : null };
  })
);

const ldb = new Database(":memory:");
applyPersonaEngineMigrations(ldb);
const lpersonas = new PersonasRepo(ldb);
lpersonas.bulkSeed(DISCUSSION_PERSONA_SEEDS);
const lrules = new RulesRepo(ldb);
lrules.log({ rule_id: "propose-on-gap", action: "fire", actor: "ai", detail: "persona advocate proposed hypothesis h-0000000001" });
lrules.log({ rule_id: "propose-on-gap", action: "fire", actor: "ai", detail: "persona advocate proposed hypothesis h-2" });
lrules.log({ rule_id: "propose-on-gap", action: "fire", actor: "ai", detail: "persona sceptic proposed hypothesis h-3" });
lrules.log({ rule_id: "refute-cold", action: "skip", actor: "engine", detail: "cooldown" });
lrules.log({ rule_id: "refute-cold", action: "error", actor: "ai", detail: "invalid JSON" });
lrules.log({ rule_id: "x-rule", action: "fire", actor: "ai", detail: "persona ghost proposed hypothesis h-4" });
lrules.log({ rule_id: null, action: "fire", actor: "ai", detail: "no rule id" });
const outcomes = [
  recordHypothesisOutcome({ personas: lpersonas, rules: lrules, hypothesisId: "h-0000000001", outcome: "integrated", statement: "選択肢を2つに絞る".repeat(10) }),
  recordHypothesisOutcome({ personas: lpersonas, rules: lrules, hypothesisId: "h-2", outcome: "rejected" }),
  recordHypothesisOutcome({ personas: lpersonas, rules: lrules, hypothesisId: "h-2", outcome: "integrated", statement: "" }),
  recordHypothesisOutcome({ personas: lpersonas, rules: lrules, hypothesisId: "h-4", outcome: "integrated" }),
  recordHypothesisOutcome({ personas: lpersonas, rules: lrules, hypothesisId: "h-none", outcome: "integrated" }),
];
const statusOf = (h: string) => (h === "h-0000000001" ? "integrated" : h === "h-2" ? "rejected" : "pending") as "integrated" | "rejected" | "pending";
writeFixture("learning.json", {
  outcomes,
  advocateNotes: JSON.parse(lpersonas.get("advocate")!.learned_notes).map((n: { note: string }) => n.note),
  personaMetrics: personaMetrics(lpersonas, lrules, statusOf),
  ruleMetrics: ruleMetrics(lrules),
});
ldb.close();

// ─── 画面: worker-pool 制御 HTML (そのまま) ──────────────────────────────────
const assets: Array<{ name: string; html: string }> = [];
assets.push({ name: "api/worker-pool-control.html", html: await (await workerPoolControlRoutes.request("/worker-pool")).text() });

// ─── 画面: 使い方ガイド (config の値を {{名前}} の印にした雛形 + 実際の出力の記録) ─────────────
type DiscordCfg = Parameters<typeof createGuideRoutes>[0];
const guideConfig = (over: {
  enabled: boolean;
  improvement: string[];
  fun: string[];
  summary: string;
  managed: string;
  learning: string;
  fbEnabled: boolean;
  fbCategory: string;
  channels: string[];
}): DiscordCfg =>
  ({
    discussionChannelIds: over.channels,
    forum: {
      enabled: over.enabled,
      summaryChannelName: over.summary,
      dataLearningChannelName: over.learning,
      managedCategoryName: over.managed,
      improvementTagNames: over.improvement,
      funTagNames: over.fun,
      discussionForumName: "議論",
    },
    gameFeedback: { enabled: over.fbEnabled, categoryName: over.fbCategory },
  }) as unknown as DiscordCfg;
const guideHtml = async (cfg: DiscordCfg) => (await createGuideRoutes(cfg).request("/guide")).text();

let template = await guideHtml(
  guideConfig({
    enabled: true,
    improvement: ["@@IMP@@"],
    fun: ["@@FUN@@"],
    summary: "@@SUMMARY@@",
    managed: "@@MANAGED@@",
    learning: "@@LEARNING@@",
    fbEnabled: true,
    fbCategory: "@@FBCAT@@",
    channels: ["a", "b", "c", "d", "e", "f", "g"],
  })
);
const replaceExactly = (from: string, to: string, times: number) => {
  const parts = template.split(from);
  if (parts.length - 1 !== times) throw new Error(`template marker ${from}: expected ${times}, got ${parts.length - 1}`);
  template = parts.join(to);
};
replaceExactly("<code>@@IMP@@</code>", "{{improvementTagPills}}", 1);
replaceExactly("<code>@@FUN@@</code>", "{{funTagPills}}", 1);
replaceExactly("@@SUMMARY@@", "{{summaryChannelName}}", 2);
replaceExactly("@@MANAGED@@", "{{managedCategoryName}}", 1);
replaceExactly("@@LEARNING@@", "{{dataLearningChannelName}}", 2);
replaceExactly("@@FBCAT@@", "{{gameFeedbackCategoryName}}", 1);
replaceExactly("forum.enabled = ON）", "forum.enabled = {{forumEnabled}}）", 1);
replaceExactly("⑤ ゲーム感想チャンネル（匿名収集）</h2>", "⑤ ゲーム感想チャンネル（匿名収集）{{gameFeedbackDisabled}}</h2>", 1);
replaceExactly("<b>7 件</b>", "<b>{{plainChannelCount}} 件</b>", 1);
if (template.includes("@@")) throw new Error("unreplaced marker");
assets.push({ name: "api/guide.html", html: template });

const guideCases = [
  { enabled: true, improvement: ["改善提案", "改善"], fun: ["面白さ"], summary: "まとめ投稿", managed: "Discutere管理", learning: "データ学習依頼", fbEnabled: true, fbCategory: "ゲーム感想", channels: ["1", "", "2"] },
  { enabled: false, improvement: [], fun: [], summary: "<b>&\"'x", managed: "a&b", learning: "<script>", fbEnabled: false, fbCategory: "\"q\"", channels: [] },
  { enabled: true, improvement: ["<i>", "a&b", "x\"y"], fun: ["{{summaryChannelName}}"], summary: "{{funTagPills}}", managed: "m", learning: "l", fbEnabled: true, fbCategory: "c", channels: ["x"] },
];
writeFixture(
  "guide.json",
  await Promise.all(guideCases.map(async (c) => ({ config: c, html: await guideHtml(guideConfig(c)) })))
);

for (const { name, html } of assets) {
  const file = path.join(nativeDir, "web", name);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, html);
}
writeFixture(
  "web_assets.json",
  assets.map(({ name, html }) => ({ name, bytes: Buffer.byteLength(html), sha256: crypto.createHash("sha256").update(html).digest("hex") }))
);

// ─── 外部会話への参加 (LLM に渡す system / prompt と結果) ─────────────────────────
const externalCases = [
  { name: "quiet pro", rngSeq: [0, 0], text: JSON.stringify({ text: " 段階的に試すと影響を確認できそうです。 " }), messages: [{ id: "1", kind: "human", text: "どう進めよう", at: 1000 }] },
  { name: "quiet con", rngSeq: [0, 0.9], text: JSON.stringify({ text: "懸念があります" }), messages: [{ id: "1", kind: "human", text: "案A\n\"引用\"", at: 1000 }] },
  { name: "empty text", rngSeq: [0, 0.2], text: '{"text":""}', messages: [{ id: "1", kind: "human", text: "x", at: 1000 }] },
  { name: "skip draw", rngSeq: [0.99], text: "{}", messages: [{ id: "1", kind: "human", text: "x", at: 1000 }] },
  { name: "active", rngSeq: [0.1, 0.1], text: '{"text":"ok"}', messages: [0, 1, 2].map((i) => ({ id: String(i), kind: "human", text: "検討中", at: 1_000_000 - 1000 + i })) },
  { name: "too long", rngSeq: [0, 0], text: JSON.stringify({ text: "あ".repeat(1201) }), messages: [{ id: "1", kind: "human", text: "x", at: 1000 }] },
  { name: "exactly 1200", rngSeq: [0, 0], text: JSON.stringify({ text: "あ".repeat(1200) }), messages: [{ id: "1", kind: "human", text: "x", at: 1000 }] },
  { name: "not json", rngSeq: [0, 0], text: "nope", messages: [{ id: "1", kind: "human", text: "x", at: 1000 }] },
  { name: "array", rngSeq: [0, 0], text: "[\"text\"]", messages: [{ id: "1", kind: "human", text: "x", at: 1000 }] },
  { name: "llm fail", rngSeq: [0, 0], fail: true, text: "", messages: [{ id: "1", kind: "human", text: "x", at: 1000 }] },
];
const externalResults = [];
for (const ec of externalCases) {
  const db = new Database(":memory:");
  let i = 0;
  const rng = () => ec.rngSeq[Math.min(i++, ec.rngSeq.length - 1)];
  const calls: unknown[] = [];
  const service = new ExternalDiscussionService(
    new ParticipationStore(db),
    {
      async invoke(args) {
        calls.push(args);
        return ec.fail ? { ok: false, error: "boom" } : { ok: true, text: ec.text };
      },
    },
    "configured-model",
    () => 1_000_000,
    rng
  );
  const input = parseConversation({ scene: "actio:team:thread", enabled: true, messages: ec.messages });
  let result: unknown;
  let error: string | null = null;
  try {
    result = await service.consider(input);
  } catch (e) {
    error = (e as Error).message;
  }
  const rows = db.prepare("SELECT id, state, result FROM external_discussion_attempts").all();
  externalResults.push({ name: ec.name, rngSeq: ec.rngSeq, llmText: ec.text, fail: !!ec.fail, messages: ec.messages, calls, result: result ?? null, error, rows });
  db.close();
}
writeFixture("external_service.json", externalResults);
