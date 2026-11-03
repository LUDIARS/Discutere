// w4-P 担当 (dialectic エンジンの駆動・4 フローの振り分け・未来予測・改善・壁打ち) の Node 実装の入出力を記録する。
// 実行: npx tsx native/tests/golden/w4-P-flow.gen.ts → native/tests/fixtures/flow/w4p_golden.json
//
// 純関数 (vote-display / dialectic conclusion / foresight camps・split / parseFlowKind / pickSideSpeaker / ensureDebaters) は
// 入出力をそのまま記録する。
// 進行 (runDialecticFlow / dispatchFlow / runImprovementFlow / SparringSession / runForesightPhase) は、プロンプトの印で
// 応答を決める偽の LLM (tests/flow/dialectic-driver.test.ts のルーターと同じ) で最後まで回し、LLM への呼び出し列
// (system / prompt / model / maxTokens) と応答、DB に残る全行 (時刻の列を除く)、通知、log / warn、戻り値を記録する。
// C++ は記録した応答を順に返して同じものを作る (w3l_support.hpp の ReplayLLM)。
// UUID (randomUUID) は出てきた順に <U1> <U2> ... へ置き換える。乱数は両側同じ LCG を注入する。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "w4p-golden-"));
process.env.DISCUTERE_CONFIG = path.join(tmp, "no-config.json");
process.env.DATABASE_PATH = path.join(tmp, "init.db");

const UUID_RE = /[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}/g;

class Normalizer {
  map = new Map<string, string>();
  str(s: string): string {
    return s.replace(UUID_RE, (m) => {
      let v = this.map.get(m);
      if (!v) {
        v = `<U${this.map.size + 1}>`;
        this.map.set(m, v);
      }
      return v;
    });
  }
  any(v: unknown): unknown {
    if (typeof v === "string") return this.str(v);
    if (Array.isArray(v)) return v.map((x) => this.any(x));
    if (v && typeof v === "object") {
      const o: Record<string, unknown> = {};
      for (const [k, x] of Object.entries(v as Record<string, unknown>)) o[this.str(k)] = this.any(x);
      return o;
    }
    return v;
  }
}

function lcg(seed: number) {
  let s = seed;
  return () => {
    s = (s * 1664525 + 1013904223) & 0xffffffff;
    return (s >>> 0) / 0x100000000;
  };
}

type Resp = { ok: true; text: string } | { ok: false; error: string };
type Responder = (p: string, sys: string) => string | Resp;

function extractGroundIds(prompt: string): string[] {
  const ids = new Set<string>();
  for (const m of prompt.matchAll(/\[((?:pro|con)-G\d+)\]/g)) ids.add(m[1]);
  return [...ids];
}

function extractOwnGroundIds(prompt: string): string[] {
  const m = prompt.match(/根拠は \[([^\]]+)\] だった/);
  return m ? m[1].split(",").map((s) => s.trim()).filter(Boolean) : [];
}

/** プロンプトの印で応答を決める偽の LLM (dialectic-driver.test.ts のルーター + rounds / 未来予測の段)。 */
function routerLlm(norm: Normalizer, overrides: Record<string, Responder> = {}) {
  const calls: unknown[] = [];
  const counters: Record<string, number> = {};
  const routes: Array<{ name: string; match: (p: string, sys: string) => boolean; respond: Responder }> = [
    { name: "foresight-proposal", match: (_p, s) => s.includes("破壊可能な一手を検討する"), respond: () => "【破壊可能な一手】無料で配る" },
    {
      name: "foresight-forecast",
      match: (_p, s) => s.includes("条件付きのシナリオで示す"),
      respond: () => "【未解決の前提】a\n【3年後】b\n【5年後】c\n【10年後】d\n【ビジネス分析】算定不能",
    },
    {
      name: "foresight-camp",
      match: (_p, s) => s.includes("【改革派 (賛同)】") || s.includes("【保守派 (否定)】"),
      respond: (_p, s) => (s.includes("【改革派") ? "壊せる" : "守るべき"),
    },
    { name: "persona-setup", match: (p) => p.includes("重視する価値 (価値軸)"), respond: () => "[]" },
    {
      name: "issue-decompose",
      match: (p) => p.includes("賛否が本気で割れる論点"),
      respond: () => '["論点その1は妥当か?", "論点その2は妥当か?", "論点その3は妥当か?"]',
    },
    { name: "proxy-stance", match: (p) => p.includes("この人物がこの論点に賛成するか反対するか"), respond: () => "pro" },
    {
      name: "position",
      match: (p) => p.includes("定立 (Position) を張ります"),
      respond: (p) =>
        JSON.stringify({
          claim: p.includes("(pro)") ? "賛成の主張" : "反対の主張",
          grounds: ["根拠その1", "根拠その2"],
          values: ["価値前提"],
          text: p.includes("(pro)") ? "賛成だよ、理由は2つある。" : "反対だね、こっちも理由がある。",
        }),
    },
    { name: "facilitator", match: (p) => p.includes("あなたは議論の進行役です。次の進行内容"), respond: () => "では続けましょう。" },
    {
      name: "turn-concede",
      match: (p) => p.includes("rebut | concede") && p.includes("最後の反論に答えて") && p.includes("- 反論 2 "),
      respond: () => JSON.stringify({ act: "concede", text: "そこは認めるよ。" }),
    },
    {
      name: "turn-respond",
      match: (p) => p.includes("rebut | concede") && p.includes("最後の反論に答えて"),
      respond: () =>
        JSON.stringify({
          act: "rebut",
          attack: "data",
          kind: "induction",
          data: "その事例は天井の仕様が違うゲームだった",
          warrant: "仕様が違う事例からは継続率の話は言えない",
          text: "いや、その反論にはこう返せる。",
        }),
    },
    {
      name: "turn-attack",
      match: (p) => p.includes("rebut | concede") && p.includes("最後の論拠に答えて"),
      respond: () =>
        JSON.stringify({
          act: "rebut",
          attack: "warrant",
          kind: "induction",
          data: "似た緩和をしたゲームでは継続率が変わらなかった",
          warrant: "主因が天井でないなら緩和しても継続しない",
          text: "その根拠は怪しいと思う。",
        }),
    },
    { name: "rule-check", match: (p) => p.includes("前提ルールを守っているかを判定"), respond: () => "ok" },
    {
      name: "position-stance",
      match: (p) => p.includes("「はい」と答えているか「いいえ」と答えているか"),
      respond: (p) => (p.includes("賛成の主張") ? "yes" : "no"),
    },
    { name: "concession", match: (p) => p.includes("返答は相手の発言を認めているか"), respond: () => "concede" },
    { name: "tension-classify", match: (p) => p.includes("この対立の「型」を 1 つだけ選んで"), respond: () => "values" },
    { name: "fact-resolve", match: (p) => p.includes("証拠はどちらの主張を支持しますか"), respond: () => "A" },
    {
      name: "synthesize",
      match: (p) => p.includes("止揚 (aufheben) の 3 要件"),
      respond: (p) =>
        JSON.stringify({
          text: "コア層は現状維持、ライト層は緩和という条件分割でいこう。",
          preserves: extractGroundIds(p),
          negates: [],
          elevates: "コア層とライト層で適用条件を分割する新しい枠",
        }),
    },
    { name: "elevation-gate", match: (p) => p.includes("統合案の「新しい枠」"), respond: () => "aufheben" },
    {
      name: "ratify",
      match: (p) => p.includes("チェックリスト (敵対的批准"),
      respond: (p) =>
        JSON.stringify({ verdict: "accept", preservedIds: extractOwnGroundIds(p), missing: [], reason: "根拠が保存されている" }),
    },
    {
      name: "vote",
      match: (p) => p.includes("あなたは中立な審判として"),
      respond: (p) => {
        const ids = [...p.matchAll(/\[ID:([^\]]+)\]/g)].map((m) => m[1]).sort();
        return ids[0] ?? "";
      },
    },
    {
      name: "effect-predict",
      match: (p) => p.includes("変化しそうな次元"),
      respond: (p) =>
        p.includes("発話A")
          ? '{"changes":[{"dim":"emo.valence","delta":0.2,"reason":"軽微な改善"}],"confidence":"high"}'
          : '{"changes":[{"dim":"emo.valence","delta":0.8,"reason":"体験が大きく改善"}],"confidence":"high"}',
    },
    { name: "summary", match: (p) => p.includes("このラウンドの議論を 2〜3 文で要約"), respond: () => "ラウンドの要約です。" },
    { name: "aufhebung", match: (p) => p.includes("found=false"), respond: () => '{"found":false,"summary":""}' },
    {
      name: "conclusion",
      match: (p, s) => p.includes('"summary"') || p.includes("締めのまとめ") || s.includes("締めのまとめ"),
      respond: () => '{"summary": "議論の到達点まとめ"}',
    },
    { name: "facilitator-rounds", match: (p) => p.startsWith("あなたは議論の進行役です。\nテーマ"), respond: () => "ラウンドの議題を提示します。" },
    { name: "persona", match: () => true, respond: () => "一般的な意見です。" },
  ];
  const stats: Record<string, number> = {};
  return {
    calls,
    stats,
    llm: {
      async invoke(args: { system?: string; prompt: string; model?: string; maxTokens?: number }) {
        const p = args.prompt;
        const sys = args.system ?? "";
        const route = routes.find((r) => r.match(p, sys))!;
        stats[route.name] = (stats[route.name] ?? 0) + 1;
        counters[route.name] = (counters[route.name] ?? 0) + 1;
        const responder = overrides[route.name] ?? route.respond;
        const raw = responder(p, sys);
        const r: Resp = typeof raw === "string" ? { ok: true, text: raw } : raw;
        calls.push({
          kind: route.name,
          system: args.system === undefined ? null : norm.str(args.system),
          prompt: norm.str(p),
          model: args.model ?? null,
          maxTokens: args.maxTokens ?? null,
          response: r.ok ? { ok: true, text: norm.str(r.text) } : { ok: false, error: r.error },
        });
        return r;
      },
    },
  };
}

const TIME_COL = /(_at|latency_ms)$/;

function dumpDb(dbPath: string, norm: Normalizer, Database: typeof import("better-sqlite3")) {
  const db = new Database(dbPath, { readonly: true });
  const tables = (db.prepare("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name").all() as Array<{ name: string }>).map((t) => t.name);
  const out: Record<string, unknown[]> = {};
  for (const t of tables) {
    if (t === "_flow_migrations" || t === "sqlite_sequence") continue;
    const rows = db.prepare(`SELECT * FROM "${t}" ORDER BY rowid`).all() as Array<Record<string, unknown>>;
    if (rows.length === 0) continue;
    out[t] = rows.map((r) => {
      const o: Record<string, unknown> = {};
      for (const [k, v] of Object.entries(r)) if (!TIME_COL.test(k)) o[k] = v;
      return norm.any(o);
    });
  }
  db.close();
  return out;
}

const ALL_ENV = [
  "DISCUTERE_FLOW_ROUNDS",
  "DISCUTERE_FLOW_TURNS_PER_ROUND",
  "DISCUTERE_FLOW_PERSONA_COUNT",
  "DISCUTERE_FLOW_VOTER_COUNT",
  "DISCUTERE_FLOW_ROSTER_FACILITATOR",
  "DISCUTERE_FLOW_ROSTER_DISCUSSANTS",
  "DISCUTERE_FLOW_DIALECTIC_JUDGE_MODEL",
  "DISCUTERE_FLOW_DIALECTIC_FINAL_JUDGE_MODEL",
  "DISCUTERE_FLOW_DIALECTIC_EXCHANGE_TURNS",
  "DISCUTERE_FLOW_ENGINE",
  "DISCUTERE_FLOW_SPARRING_MAX_TURNS",
  "DISCUTERE_FLOW_FORESIGHT_ENABLED",
  "DISCUTERE_FLOW_FORESIGHT_TURNS",
  "DISCUTERE_FLOW_FORESIGHT_MODEL",
  "DISCUTERE_FLOW_FORESIGHT_FORECAST_MODEL",
  "DISCUTERE_FLOW_IMPROVEMENT_EFFECT_MODEL",
  "DISCUTERE_FACILITATOR_AUFHEBUNG_TARGET",
];

const DIALECTIC_ENV = {
  DISCUTERE_FLOW_PERSONA_COUNT: "4",
  DISCUTERE_FLOW_VOTER_COUNT: "3",
  DISCUTERE_FLOW_ROSTER_FACILITATOR: "gpt-facilitator@high",
  DISCUTERE_FLOW_ROSTER_DISCUSSANTS: "claude-pro@high,gpt-con@medium,claude-opinion@low",
  DISCUTERE_FLOW_DIALECTIC_JUDGE_MODEL: "claude-judge@low",
  DISCUTERE_FLOW_DIALECTIC_FINAL_JUDGE_MODEL: "claude-final-judge@high",
};

const LABEL_A = "論者#aaa111";
const LABEL_B = "論者#bbb222";
function proxyPersona(label: string, key: string, model: string) {
  return {
    id: `proxy:${key}:rpg`,
    name: label,
    role: "debater",
    stance: "pro",
    speechStyle: "本人の発言に近い口語",
    traits: ["公開レビュー由来の AI 代理", "ジャンル rpg"],
    model,
    isLocal: false,
    possession: { label: "AI代理" },
    proxy: {
      label,
      speakerKey: key,
      genre: "rpg",
      bundleText: `代理する人物: ${label}\n[E1] 日課の義務は嫌だと言っている\n[E2] 課金は悪くないと言っている`,
      evidenceIds: ["E1", "E2"],
      evidenceTexts: { E1: "日課の義務は嫌だと言っている", E2: "課金は悪くないと言っている" },
    },
  };
}

async function main() {
  const Database = (await import("better-sqlite3")).default;
  const { _resetConfig } = await import("../../../src/config.js");
  const { _resetFlowDb } = await import("../../../src/flow/db/connection.js");
  const vd = await import("../../../src/flow/vote-display.js");
  const dc = await import("../../../src/flow/dialectic/conclusion.js");
  const { pickSideSpeaker } = await import("../../../src/flow/dialectic/exchange-runner.js");
  const { runDialecticFlow, ensureDebaters } = await import("../../../src/flow/dialectic/driver.js");
  const { dispatchFlow, parseFlowKind } = await import("../../../src/flow/dispatch.js");
  const { runImprovementFlow } = await import("../../../src/flow/improvement.js");
  const { SparringSession } = await import("../../../src/flow/sparring.js");
  const camps = await import("../../../src/flow/foresight/camps.js");
  const phase = await import("../../../src/flow/foresight/phase.js");
  const { getForesightStatus, _resetForesightStatus } = await import("../../../src/flow/foresight/state.js");
  const { negativeBaselineVector, buildTargetVector, computeDesignGap } = await import("../../../src/flow/design-gap.js");

  const out: Record<string, unknown> = {};

  // ── vote-display ──
  const events = [
    { kind: "scores", tally: { "u-aaa": 1.2345, "u-bbb": -0.5, "u-ccc": 1.2345 }, winner: "u-aaa" },
    { kind: "scores", tally: {}, winner: null },
    { kind: "votes", tally: { "u-aaa": 2, "u-bbb": 0, "u-ccc": 3, "u-ddd": 3 }, winner: "u-ccc" },
    { kind: "votes", tally: { "u-aaa": 0 }, winner: null },
    { kind: "votes", tally: { x: 1.5, y: 0.005 }, winner: "y" },
    { kind: "scores", tally: { a: 0.005, b: -0.125, c: 2.675 }, winner: null },
  ] as const;
  out.voteDisplay = events.map((e) => ({
    event: e,
    lines: vd.buildVoteRankingLines(e as never, (id) => `名-${id}`),
    visible: vd.hasVisibleVoteResult(e as never),
    stack: vd.shouldStackVoteReactions(e.kind),
    title: vd.voteNoticeTitle(3, e.kind),
  }));

  // ── dialectic conclusion (決着表) ──
  const issue = { id: "i1", sessionId: "s", title: "天井を下げるべきか", ordinal: 2, source: "llm", status: "concluded" };
  const positions = [
    { id: "a", issueId: "i1", personaId: "p-pro", stance: "pro", claim: "下げる", qualifier: null, grounds: [], values: [] },
    { id: "b", issueId: "i1", personaId: "p-unknown", stance: "con", claim: "下げない", qualifier: null, grounds: [], values: [] },
  ];
  const t = (id: string, status: string, note: string | null = null) => ({
    id,
    issueId: "i1",
    positionA: "a",
    positionB: "b",
    type: "values",
    status,
    resolutionNote: note,
  });
  const syn = (tensionId: string, text: string, elevates: string | null) => ({
    id: `s-${tensionId}`,
    tensionId,
    preserves: [],
    negates: [],
    elevates,
    kind: "aufheben",
    text,
    ratification: [],
    status: "ratified",
    revision: 0,
  });
  const settlements = [
    { issue, positions, tensions: [t("t1", "synthesized")], syntheses: [syn("t1", "層で分ける", "新しい枠")], refutation: null },
    { issue, positions, tensions: [t("t1", "synthesized")], syntheses: [], refutation: null },
    { issue, positions, tensions: [t("t1", "synthesized"), t("t2", "compromised")], syntheses: [syn("t2", "折衷", null), syn("t1", "", "")], refutation: null },
    { issue, positions, tensions: [], syntheses: [], refutation: "「下げない」は論破された" },
    { issue, positions, tensions: [], syntheses: [], refutation: null, undecided: "双方崩れた" },
    { issue, positions: [], tensions: [], syntheses: [], refutation: null },
    { issue, positions, tensions: [t("t1", "unresolved_fact", "売上データなし"), t("t2", "resolved", "A を支持")], syntheses: [], refutation: null },
    { issue, positions, tensions: [t("t1", "agreed_disagree"), t("t2", "open"), t("t3", "unresolved_fact", null)], syntheses: [], refutation: null },
  ];
  const names = new Map([["p-pro", "翔"], ["p-con", "凛"]]);
  out.settlements = settlements.map((s, i) => ({
    settlement: s,
    share: i % 3 === 0 ? null : i === 1 ? 0.5 : 0.675,
    summary: dc.settlementToRoundSummary(s as never, names, i % 3 === 0 ? null : i === 1 ? 0.5 : 0.675),
  }));
  out.settlementNames = Object.fromEntries(names);
  out.unresolvedFacts = dc.collectUnresolvedFacts(settlements as never);

  // ── pickSideSpeaker / ensureDebaters ──
  const mk = (id: string, stance: string, role = "debater") => ({ id, name: `名${id}`, role, stance, speechStyle: "", traits: [], model: "m", isLocal: false });
  const speakers = [mk("a", "pro"), mk("b", "con"), mk("c", "pro"), mk("o", "pro", "opinion"), mk("f", "neutral", "facilitator")];
  out.pickSide = [
    { side: "pro", counts: {} },
    { side: "pro", counts: { a: 2, c: 1 } },
    { side: "pro", counts: { a: 2, c: 2 } },
    { side: "con", counts: {} },
    { side: "neutral", counts: {} },
    { side: "opinion", counts: { o: 3 } },
    { side: "reform", counts: {} },
  ].map((c) => ({ ...c, picked: pickSideSpeaker(speakers as never, c.side as never, new Map(Object.entries(c.counts)))?.id ?? null }));
  out.speakers = speakers;
  const debaterCases = [
    [mk("f", "neutral", "facilitator"), mk("p", "pro"), mk("c", "con"), mk("o", "opinion", "opinion")],
    [mk("f", "neutral", "facilitator"), mk("p", "pro"), mk("o", "opinion", "opinion"), mk("o2", "opinion", "opinion")],
    [mk("f", "neutral", "facilitator"), mk("o", "opinion", "opinion"), mk("o2", "opinion", "opinion")],
    [mk("f", "neutral", "facilitator"), mk("o", "opinion", "opinion")],
    [mk("f", "neutral", "facilitator"), mk("d", "neutral"), mk("c", "con")],
  ];
  out.ensureDebaters = debaterCases.map((cast) => {
    const warns: string[] = [];
    try {
      const r = ensureDebaters(cast as never, (m) => warns.push(m));
      return { cast, ok: true, pro: r.pro, con: r.con, result: r.cast, warns };
    } catch (e) {
      return { cast, ok: false, error: (e as Error).message, warns };
    }
  });

  // ── parseFlowKind ──
  out.flowKinds = [
    "議論", "企画/議論", "改善/議論", "🔧 改善／議論", "discussion", "DISCUSSION ", "  Improvement", "改善", "改善提案", "学習", "壁打ち",
    "sparring", "learning", "", "   ", "雑談", "【議論】", "ディスカッションしよう", "提案です", "討論会", "企画／議論メモ", "ＤＩＳＣＵＳＳＩＯＮ",
  ].map((label) => ({ label, kind: parseFlowKind(label) }));

  // ── foresight camps / split ──
  const rec = (over: Record<string, unknown>) => ({
    id: "u", sessionId: "s-fs", paperId: "p-fs", round: 1, turn: 1, personaId: "x", personaName: "X", role: "debater", stance: "pro",
    text: "発言", isError: false, ...over,
  });
  const mainUtterances = [
    rec({ id: "u1", personaId: "f", personaName: "司会", role: "facilitator", stance: "neutral", turn: 0 }),
    rec({ id: "u2", personaId: "a", personaName: "賛成A", stance: "pro", text: "A の 1 回目" }),
    rec({ id: "u3", personaId: "b", personaName: "反対B", stance: "con", text: "B の 1 回目" }),
    rec({ id: "u4", personaId: "c", personaName: "意見C", role: "opinion", stance: "opinion", round: 2 }),
    rec({ id: "u5", personaId: "a", personaName: "賛成A", stance: "pro", round: 2, text: "A の 2 回目" }),
    rec({ id: "u6", personaId: "a", personaName: "賛成A", stance: "pro", round: 2, text: "  A の 3 回目  " }),
    rec({ id: "u7", personaId: "e", personaName: "壊れE", stance: "con", isError: true }),
  ];
  const castInputs = [
    mainUtterances,
    [rec({ personaId: "b", stance: "con" }), rec({ personaId: "a", stance: "pro" })],
    [rec({ personaId: "a", stance: "pro" })],
    [],
    [rec({ personaId: "a", text: "あ".repeat(500) }), rec({ personaId: "b", text: "😀".repeat(241) }), rec({ personaId: "c", text: "😀".repeat(240) })],
    [rec({ personaId: "f", role: "facilitator", isError: true }), rec({ personaId: "f2", personaName: "司会2", role: "facilitator" }), rec({ personaId: "a" }), rec({ personaId: "b" })],
  ];
  out.casts = castInputs.map((u) => ({ utterances: u, cast: camps.buildForesightCast(u as never) }));
  out.normalizeTurns = [1, 2, 3, 5, 6, 0, -3, 2.5, 7.9, 20].map((n) => ({ n, v: camps.normalizeCampTurns(n) }));
  out.normalizeTurnsSpecial = { nan: camps.normalizeCampTurns(Number.NaN), inf: camps.normalizeCampTurns(Number.POSITIVE_INFINITY) };
  out.stages = [0, 1, 2, 3, 4, 5, 6, 7, 8, 100].map((i) => ({ i, stage: camps.campStageAt(i) }));
  const cast3 = camps.buildForesightCast(mainUtterances as never)!;
  out.turnOrders = [1, 4, 6, 7].map((turns) => ({ turns, order: camps.buildCampTurnOrder(cast3.speakers, turns).map((s) => s.personaId) }));
  out.turnOrderOneCamp = camps.buildCampTurnOrder(cast3.speakers.filter((s) => s.camp === "reform"), 4).length;
  const longText = ["a".repeat(1000), "b".repeat(1000), "c".repeat(4000)].join("\n");
  out.splits = [
    { text: longText, limit: 1780 },
    { text: "😀".repeat(250), limit: 100 },
    { text: "\n\nabc\n\ndef", limit: 5 },
    { text: "abc\ndef\nghi", limit: 7 },
    { text: "", limit: 10 },
    { text: "x".repeat(10) + "\n" + "y".repeat(3), limit: 10 },
    { text: "line1\n" + "あ".repeat(25) + "\nline3\n", limit: 10 },
  ].map((c) => ({ ...c, chunks: phase.splitForPost(c.text, c.limit) }));
  out.labels = [[], ["x"], ["x", "y"], ["a", "b", "c"]].map((c) => ({ chunks: c, labeled: phase.labelParts(c) }));

  // ── 進行 (偽の LLM で最後まで回す) ──
  type Scenario = {
    name: string;
    mode: "dialectic" | "dispatch" | "improvement" | "sparring" | "foresight";
    env: Record<string, string>;
    theme: string;
    tags: string[];
    seed: number;
    sessionId?: string;
    flow?: string;
    voices?: Array<{ content: string; source: string; sourceUrl?: string }>;
    paperOverride?: Record<string, unknown>;
    proxy?: boolean;
    overrides?: Record<string, Responder>;
    onVoteThrows?: boolean;
    currentVector?: "baseline";
    opinionToVector?: "ab";
    userTexts?: string[];
    foresightOpts?: Record<string, unknown>;
  };
  const turnRespondForRefuted: Responder = (p) =>
    p.includes("あなたは【賛成】陣営")
      ? JSON.stringify({ act: "concede", text: "そこは認めるよ。" })
      : JSON.stringify({ act: "rebut", attack: "data", kind: "induction", data: "その事例は仕様が違うゲームだった", warrant: "仕様が違う事例からは言えない", text: "いや、その反論にはこう返せる。" });
  let stanceCalls = 0;
  let attackCalls = 0;
  let campCalls = 0;
  const scenarios: Scenario[] = [
    { name: "dlx-basic", mode: "dialectic", env: DIALECTIC_ENV, theme: "ガチャ緩和の是非", tags: [], seed: 1234, onVoteThrows: true },
    { name: "dlx-fact-unresolved", mode: "dialectic", env: DIALECTIC_ENV, theme: "売上は下がるのか", tags: [], seed: 42, voices: [], overrides: { "tension-classify": () => "facts" } },
    {
      name: "dlx-fact-resolved",
      mode: "dialectic",
      env: DIALECTIC_ENV,
      theme: "売上は下がるのか (証拠あり)",
      tags: [],
      seed: 7,
      voices: [
        { content: "緩和後も売上は維持されたという決算資料の話", source: "niconico" },
        { content: "実際 DAU が伸びて回復した", source: "youtube", sourceUrl: "https://youtu.be/x" },
      ],
      overrides: { "tension-classify": () => "facts", "fact-resolve": () => "A" },
    },
    {
      name: "dlx-reject",
      mode: "dialectic",
      env: DIALECTIC_ENV,
      theme: "批准が割れるテーマ",
      tags: [],
      seed: 99,
      overrides: {
        ratify: (p) => JSON.stringify({ verdict: "reject", preservedIds: [], missing: extractOwnGroundIds(p), reason: "自分の根拠が保存されていない" }),
      },
    },
    {
      name: "dlx-compromise",
      mode: "dialectic",
      env: DIALECTIC_ENV,
      theme: "折衷で終わるテーマ",
      tags: [],
      seed: 555,
      overrides: { synthesize: (p) => JSON.stringify({ text: "両方の意見をそのまま採用しよう。", preserves: extractGroundIds(p), negates: [], elevates: "" }) },
    },
    { name: "dlx-refuted", mode: "dialectic", env: DIALECTIC_ENV, theme: "論破されるテーマ", tags: [], seed: 5, overrides: { "turn-respond": turnRespondForRefuted } },
    {
      name: "dlx-both-fallen",
      mode: "dialectic",
      env: DIALECTIC_ENV,
      theme: "双方崩れるテーマ",
      tags: [],
      seed: 6,
      overrides: { "turn-respond": () => JSON.stringify({ act: "concede", text: "そこは認めるよ。" }) },
    },
    {
      name: "dlx-human-issues",
      mode: "dialectic",
      env: DIALECTIC_ENV,
      theme: "人が論点を決めたテーマ",
      tags: [],
      seed: 1234,
      paperOverride: { mechanics: [], supplement: "", issues: ["論点A は十分か", " ", "論点B は正しいか", "論点C は正解か"], debatability: { debatable: false, armableBothCount: 0 } },
    },
    {
      name: "dlx-stance",
      mode: "dialectic",
      env: DIALECTIC_ENV,
      theme: "立場のテーマ",
      tags: [],
      seed: 7,
      overrides: {
        "position-stance": (p) => {
          stanceCalls += 1;
          if (p.includes("反対の主張") && stanceCalls <= 2) return "yes";
          return p.includes("賛成の主張") ? "yes" : "no";
        },
        concession: () => "rebut",
      },
    },
    {
      name: "dlx-foul-nodata",
      mode: "dialectic",
      env: DIALECTIC_ENV,
      theme: "前提ルールのテーマ",
      tags: [],
      seed: 7,
      overrides: { "turn-attack": () => JSON.stringify({ act: "rebut", target: null, attack: "data", text: "それって感想だよね。" }) },
    },
    {
      name: "dlx-foul-noattack",
      mode: "dialectic",
      env: DIALECTIC_ENV,
      theme: "前提ルールのテーマ",
      tags: [],
      seed: 7,
      overrides: { "turn-attack": () => JSON.stringify({ act: "rebut", target: null, data: "事例がある", warrant: "だから違う", text: "違うと思う。" }) },
    },
    { name: "dlx-foul-scope", mode: "dialectic", env: DIALECTIC_ENV, theme: "前提ルールのテーマ", tags: [], seed: 7, overrides: { "rule-check": () => "out_of_scope" } },
    {
      // 論点分解の LLM 失敗 → テーマ 1 論点 / 応酬の LLM エラー・空応答 / 応答ターン上限 / 開幕の LLM エラー
      name: "dlx-degrade",
      mode: "dialectic",
      env: { ...DIALECTIC_ENV, DISCUTERE_FLOW_DIALECTIC_EXCHANGE_TURNS: "3", DISCUTERE_FLOW_PERSONA_COUNT: "3" },
      theme: "壊れた LLM のテーマ",
      tags: ["開発"],
      seed: 11,
      paperOverride: { mechanics: [{ name: "天井", description: "一定回数で確定" }], supplement: "補足", debatability: { debatable: false, armableBothCount: 1 } },
      overrides: {
        "issue-decompose": () => ({ ok: false, error: "decompose down" }),
        "turn-attack": () => {
          attackCalls += 1;
          if (attackCalls === 1) return { ok: false, error: "rate limited" };
          if (attackCalls === 2) return "   ";
          return JSON.stringify({ act: "rebut", attack: "warrant", kind: "induction", data: "反例がある", warrant: "だから成り立たない", text: "反例がある。" });
        },
        facilitator: (p) => (p.includes("について議論します") ? { ok: false, error: "facilitator down" } : "では続けましょう。"),
      },
    },
    {
      name: "proxy-e2e",
      mode: "dialectic",
      env: { ...DIALECTIC_ENV, DISCUTERE_FLOW_ROSTER_DISCUSSANTS: "claude-a@high,claude-b@high" },
      theme: "周回報酬は必要か",
      tags: ["代理"],
      seed: 3,
      proxy: true,
      paperOverride: { mechanics: [], supplement: "", issues: ["論点その1は妥当か?", "論点その2は妥当か?"] },
      overrides: {
        "proxy-stance": (p) => {
          const who = p.includes(`代理する人物: ${LABEL_A}`) ? "A" : "B";
          if (p.includes("論点その1")) return who === "A" ? "pro" : "con";
          return "pro";
        },
        position: (p) =>
          JSON.stringify({
            claim: p.includes("(pro)") ? "賛成の主張" : "反対の主張",
            grounds: [
              { kind: "induction", data: "日課の義務は嫌だと言っている", warrant: "義務化は続ける気を削ぐ", say: "義務になるのは嫌なんだ。", claimKind: "groundedParaphrase", evidenceIds: ["E1"] },
              { kind: "abduction", data: "本人は「課金は悪」と言っている", warrant: "x", say: "課金も嫌だね。", claimKind: "sourceQuote", evidenceIds: ["E2"] },
            ],
            values: ["体験"],
            text: p.includes("(pro)") ? "賛成だ。" : "反対だ。",
          }),
        "turn-attack": () =>
          JSON.stringify({ act: "rebut", attack: "data", kind: "induction", data: "別のデータ", warrant: "崩れる", text: "それは違うと思う。", claimKind: "groundedParaphrase", evidenceIds: ["E9"] }),
        "turn-respond": () => JSON.stringify({ act: "concede", text: "認める。" }),
        "turn-concede": () => JSON.stringify({ act: "concede", text: "認める。" }),
        synthesize: () => JSON.stringify({ text: "条件で分ける", preserves: [], negates: [], elevates: "" }),
        ratify: () => JSON.stringify({ verdict: "reject", preservedIds: [], missing: [], reason: "x" }),
        conclusion: () => '{"summary": "まとめ"}',
      },
    },
    {
      // dispatch: engine=dialectic + 未来予測 (本議論 → 未来予測の後段)
      name: "dispatch-dialectic-foresight",
      mode: "dispatch",
      env: { ...DIALECTIC_ENV, DISCUTERE_FLOW_ENGINE: "dialectic", DISCUTERE_FLOW_FORESIGHT_TURNS: "3", DISCUTERE_FLOW_FORESIGHT_FORECAST_MODEL: "forecast-model" },
      theme: "エンジン分岐テスト",
      tags: ["未来予測"],
      flow: "議論",
      seed: 2026,
      sessionId: "dlx-dispatch",
      voices: [{ content: "声その1".repeat(60), source: "niconico" }],
      paperOverride: { mechanics: [], supplement: "", issues: ["論点X は妥当か"] },
      overrides: {
        "foresight-camp": (_p, s) => {
          campCalls += 1;
          if (campCalls === 2) return { ok: false, error: "camp-boom" };
          return s.includes("【改革派") ? "壊せる" : "守るべき";
        },
      },
    },
    {
      // dispatch: engine=rounds の改善 + 未来予測 + 機密 (外部の声を渡さない・現状ベクトルはベースライン)
      name: "dispatch-improvement-confidential",
      mode: "dispatch",
      env: {
        DISCUTERE_FLOW_ENGINE: "rounds",
        DISCUTERE_FLOW_ROUNDS: "1",
        DISCUTERE_FLOW_TURNS_PER_ROUND: "2",
        DISCUTERE_FLOW_PERSONA_COUNT: "3",
        DISCUTERE_FLOW_IMPROVEMENT_EFFECT_MODEL: "effect-model",
        DISCUTERE_FLOW_FORESIGHT_MODEL: "fs-model",
      },
      theme: "改善テストテーマ",
      tags: ["未来予測", "機密"],
      flow: "改善/議論",
      seed: 77,
      sessionId: "imp-dispatch",
      voices: [{ content: "渡してはいけない声", source: "steam" }],
      overrides: { persona: () => ((attackCalls++ % 2 === 0) ? "発話A です" : "発話B です") },
    },
    {
      name: "improvement-effect",
      mode: "improvement",
      env: { DISCUTERE_FLOW_ENGINE: "rounds", DISCUTERE_FLOW_ROUNDS: "1", DISCUTERE_FLOW_TURNS_PER_ROUND: "2", DISCUTERE_FLOW_PERSONA_COUNT: "2" },
      theme: "改善テストテーマ",
      tags: [],
      seed: 8,
      currentVector: "baseline",
      overrides: { persona: () => ((attackCalls++ % 2 === 0) ? "発話A" : "発話B") },
    },
    {
      name: "improvement-fallback",
      mode: "improvement",
      env: { DISCUTERE_FLOW_ENGINE: "rounds", DISCUTERE_FLOW_ROUNDS: "1", DISCUTERE_FLOW_TURNS_PER_ROUND: "2", DISCUTERE_FLOW_PERSONA_COUNT: "2" },
      theme: "改善テストテーマ",
      tags: [],
      seed: 9,
      currentVector: "baseline",
      opinionToVector: "ab",
      onVoteThrows: true,
      overrides: {
        persona: () => ((attackCalls++ % 2 === 0) ? "発話A" : "発話B"),
        "effect-predict": () => ({ ok: false, error: "effect-predict down" }),
      },
    },
    {
      name: "sparring",
      mode: "sparring",
      env: { DISCUTERE_FLOW_PERSONA_COUNT: "2", DISCUTERE_FLOW_SPARRING_MAX_TURNS: "3" },
      theme: "壁打ちテーマ",
      tags: ["機密"],
      seed: 99,
      voices: [{ content: "声", source: "steam" }],
      userTexts: ["最初の意見です", "二つ目の意見", "まとめてください", "続きの意見", "おわり", "まだ話したい"],
      overrides: {
        persona: () => {
          campCalls += 1;
          if (campCalls % 3 === 0) return { ok: false, error: "spar down" };
          if (campCalls % 4 === 0) return "  ";
          return "なるほど、その点は重要ですね。";
        },
        aufhebung: () => (campCalls > 3 ? '{"found":true,"summary":"統合した視点"}' : '{"found":false,"summary":""}'),
      },
    },
    {
      name: "sparring-guard",
      mode: "sparring",
      env: { DISCUTERE_FLOW_PERSONA_COUNT: "2", DISCUTERE_FLOW_SPARRING_MAX_TURNS: "2" },
      theme: "ガードテーマ",
      tags: [],
      seed: 7,
      userTexts: ["1", "2", "3", "4"],
    },
    {
      name: "foresight-phase",
      mode: "foresight",
      env: {},
      theme: "ガチャ課金の未来",
      tags: [],
      seed: 1,
      foresightOpts: { turns: 4, model: "m-camp", forecastModel: "m-forecast", context: { paperMd: "# 議題\n資料あり", voices: [{ content: "高すぎる", source: "steam" }] }, body: "# 議題\nX" },
      overrides: {
        "foresight-camp": (_p, s) => {
          campCalls += 1;
          if (campCalls === 1) return { ok: false, error: "camp-boom" };
          return s.includes("【改革派") ? "壊せる" : "守るべき";
        },
      },
    },
    {
      name: "foresight-proposal-fail",
      mode: "foresight",
      env: {},
      theme: "ガチャ課金の未来",
      tags: [],
      seed: 1,
      foresightOpts: { turns: 4, conclusion: "" },
      overrides: { "foresight-proposal": () => ({ ok: false, error: "boom" }) },
    },
    {
      name: "foresight-degraded",
      mode: "foresight",
      env: {},
      theme: "ガチャ課金の未来",
      tags: [],
      seed: 1,
      foresightOpts: { turns: 1, body: "# 結論\nY" },
      overrides: {
        "foresight-camp": () => {
          campCalls += 1;
          return campCalls === 1 ? "  " : "x".repeat(2000);
        },
        "foresight-forecast": () => ({ ok: false, error: "fc-boom" }),
      },
    },
  ];

  const runs: unknown[] = [];
  for (const sc of scenarios) {
    stanceCalls = 0;
    attackCalls = 0;
    campCalls = 0;
    for (const k of ALL_ENV) delete process.env[k];
    for (const [k, v] of Object.entries(sc.env)) process.env[k] = v;
    const dbPath = path.join(tmp, `${sc.name}.db`);
    process.env.DATABASE_PATH = dbPath;
    _resetConfig();
    _resetFlowDb();
    _resetForesightStatus();
    const gamesDir = path.join(tmp, `games-${sc.name}`);
    fs.mkdirSync(gamesDir, { recursive: true });

    const norm = new Normalizer();
    const { llm, calls } = routerLlm(norm, sc.overrides ?? {});
    const logs: string[] = [];
    const warns: string[] = [];
    const utterEvents: unknown[] = [];
    const voteEvents: unknown[] = [];
    const lookups: unknown[] = [];
    const common = {
      llm,
      rng: lcg(sc.seed),
      gamesDir,
      log: (m: string) => logs.push(norm.str(m)),
      warn: (m: string) => warns.push(norm.str(m)),
      onUtterance: (u: unknown) => {
        utterEvents.push(norm.any(u));
      },
      onVote: (e: unknown) => {
        voteEvents.push(norm.any(e));
        if (sc.onVoteThrows) throw new Error("vote sink down");
      },
      ...(sc.voices
        ? {
            listExternalVoices: (terms: string[], limit: number) => {
              lookups.push({ terms, limit });
              return sc.voices!;
            },
          }
        : {}),
    };
    const record: Record<string, unknown> = {
      name: sc.name,
      mode: sc.mode,
      env: sc.env,
      theme: sc.theme,
      tags: sc.tags,
      seed: sc.seed,
      sessionId: sc.sessionId ?? sc.name,
      flow: sc.flow ?? null,
      voices: sc.voices ?? null,
      paperOverride: sc.paperOverride ?? null,
      proxyParticipants: sc.proxy ? [proxyPersona(LABEL_A, "k-a", "claude-a@high"), proxyPersona(LABEL_B, "k-b", "claude-b@high")] : null,
      onVoteThrows: !!sc.onVoteThrows,
      currentVector: sc.currentVector ?? null,
      opinionToVector: sc.opinionToVector ?? null,
      userTexts: sc.userTexts ?? null,
      foresightOpts: sc.foresightOpts ?? null,
    };

    if (sc.mode === "dialectic") {
      const result = await runDialecticFlow(sc.theme, sc.tags as never, {
        ...common,
        sessionId: sc.name,
        ...(sc.paperOverride ? { paperOverride: sc.paperOverride as never } : {}),
        ...(sc.proxy ? { proxyParticipants: record.proxyParticipants as never } : {}),
      } as never);
      record.result = norm.any(result);
    } else if (sc.mode === "dispatch") {
      const r = await dispatchFlow(
        { theme: sc.theme, tags: sc.tags as never, flow: sc.flow! },
        { ...common, sessionId: sc.sessionId, ...(sc.paperOverride ? { paperOverride: sc.paperOverride as never } : {}) } as never
      );
      record.result = norm.any({ kind: r.kind, flow: r.flow, result: (r as { result: unknown }).result });
      record.foresightStatus = getForesightStatus(sc.sessionId!) ?? null;
    } else if (sc.mode === "improvement") {
      const current = negativeBaselineVector();
      const gap = computeDesignGap(current, buildTargetVector([]));
      const aVec = current.map((v, k) => v + gap[k]);
      const bVec = current.map((v, k) => v + 2 * gap[k]);
      const result = await runImprovementFlow(sc.theme, sc.tags as never, {
        ...common,
        sessionId: sc.name,
        currentVector: current,
        ...(sc.opinionToVector ? { opinionToVector: (t: string) => (t === "発話A" ? aVec : t === "発話B" ? bVec : current.slice()) } : {}),
      } as never);
      record.result = norm.any(result);
    } else if (sc.mode === "sparring") {
      const session = new SparringSession(sc.theme, sc.tags as never, { ...common, responsesPerTurn: sc.name === "sparring" ? 2 : 1 } as never);
      await session.start();
      const turns: unknown[] = [];
      for (const text of sc.userTexts!) turns.push(norm.any(await session.submitUser(text)));
      record.turns = turns;
      record.result = norm.any({ sessionId: session.sessionId, paperId: session.paperId, ended: session.isEnded });
    } else {
      const opts = sc.foresightOpts!;
      let body = (opts.body as string | undefined) ?? null;
      const committed: unknown[] = [];
      const result = await phase.runForesightPhase({
        theme: sc.theme,
        sessionId: "s-fs",
        paperId: "p-fs",
        flow: "discussion",
        llm,
        utterances: mainUtterances as never,
        conclusion: (opts.conclusion as string | undefined) ?? "ガチャは続く",
        turns: opts.turns as number,
        model: opts.model as string | undefined,
        forecastModel: opts.forecastModel as string | undefined,
        context: opts.context as never,
        commit: async (r) => {
          committed.push(norm.any(r));
        },
        ...(body !== null ? { paperBody: { get: () => body, set: (md: string) => (body = md) } } : {}),
        log: (m) => logs.push(norm.str(m)),
        warn: (m) => warns.push(norm.str(m)),
      });
      record.result = norm.any(result);
      record.committed = committed;
      record.body = body;
      record.mainUtterances = mainUtterances;
    }
    _resetFlowDb();
    record.calls = calls;
    record.utterEvents = utterEvents;
    record.voteEvents = voteEvents;
    record.lookups = lookups;
    record.logs = logs;
    record.warns = warns;
    record.db = sc.mode === "foresight" ? {} : dumpDb(dbPath, norm, Database);
    runs.push(record);
  }
  out.runs = runs;

  const dest = path.resolve("native/tests/fixtures/flow/w4p_golden.json");
  fs.writeFileSync(dest, JSON.stringify(out) + "\n");
  console.log(`wrote ${dest}`);
  fs.rmSync(tmp, { recursive: true, force: true });
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
