// w2-G 担当 (ペーパー一式・dialectic の部品) の Node 実装の入出力を記録する。
// 実行: npx tsx native/tests/golden/w2-G-flow.gen.ts → native/tests/fixtures/flow/w2g_golden.json
// LLM は決まった応答を順に返す偽物 (呼び出しも記録する)。DB は一時ディレクトリの SQLite。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "w2g-golden-"));
process.env.DATABASE_PATH = path.join(tmp, "flow.db");
process.env.DISCUTERE_CONFIG = path.join(tmp, "no-config.json");

type Resp = { ok: true; text: string } | { ok: false; error: string };
function fakeLlm(responses: Resp[]) {
  const calls: unknown[] = [];
  let i = 0;
  return {
    calls,
    llm: {
      async invoke(args: { system?: string; prompt: string; model?: string; maxTokens?: number }) {
        calls.push({ system: args.system ?? null, prompt: args.prompt, model: args.model ?? null, maxTokens: args.maxTokens ?? null });
        const r = responses[Math.min(i, responses.length - 1)] ?? { ok: false, error: "no response" };
        i += 1;
        return r.ok ? { ok: true as const, text: r.text } : { ok: false as const, error: r.error };
      },
    },
  };
}
const ok = (text: string): Resp => ({ ok: true, text });
const ng = (error: string): Resp => ({ ok: false, error });

async function main() {
  const pm = await import("../../../src/flow/paper-markdown.js");
  const dp = await import("../../../src/flow/discussion-paper.js");
  const ep = await import("../../../src/flow/effect-predict.js");
  const me = await import("../../../src/flow/mechanic-extract.js");
  const sa = await import("../../../src/flow/spec-analyze.js");
  const vote = await import("../../../src/flow/vote.js");
  const imp = await import("../../../src/flow/improvement-score.js");
  const ps = await import("../../../src/flow/persona-setup.js");
  const rs = await import("../../../src/flow/round-summary.js");
  const pr = await import("../../../src/flow/paper-refine.js");
  const vc = await import("../../../src/flow/voice-cache.js");
  const pv = await import("../../../src/flow/paper-voices.js");
  const mix = await import("../../../src/flow/user-voices/mix.js");
  const reuse = await import("../../../src/flow/paper-reuse.js");
  const deb = await import("../../../src/flow/debatability.js");
  const issues = await import("../../../src/flow/dialectic/issues.js");
  const iv = await import("../../../src/flow/dialectic/issue-voices.js");
  const ex = await import("../../../src/flow/dialectic/exchange.js");
  const goal = await import("../../../src/flow/dialectic/goal.js");
  const oc = await import("../../../src/flow/dialectic/outcome.js");
  const pst = await import("../../../src/flow/dialectic/position-stance.js");
  const pos = await import("../../../src/flow/dialectic/position.js");
  const rc = await import("../../../src/flow/dialectic/rule-check.js");
  const syn = await import("../../../src/flow/dialectic/synthesis.js");
  const ten = await import("../../../src/flow/dialectic/tension.js");
  const tp = await import("../../../src/flow/dialectic/turn-prompt.js");
  const xp = await import("../../../src/flow/dialectic/exchange-prompt.js");
  const sch = await import("../../../src/flow/dialectic/scheduler.js");
  const pa = await import("../../../src/flow/proxy/position-attribution.js");
  const { ArgumentGraph } = await import("../../../src/flow/argument-graph.js");

  const out: Record<string, unknown> = {};

  // ── paper-markdown ──
  const mechs = [
    { name: "ガチャ", description: "ランダム排出", intended_affect: "期待", source: "curated" },
    { name: "周回", description: "同じステージを回す" },
    { name: "仮説A", description: "LLM が出した", source: "llm", intended_affect: "高揚" },
    { name: "クロール", description: "集めた", source: "crawl" },
  ];
  const contents = [
    { theme: "テーマ1", tags: [], supplement: "", mechanics: [] },
    { theme: "テーマ2", tags: ["機密"], supplement: "補足です", mechanics: mechs, issues: [" 論点1 ", "", "論点2"] },
    { theme: "固定", tags: [], supplement: "旧補足", mechanics: mechs, gameTitle: " ゲームX ", discussionContent: "内容" },
    { theme: "固定2", tags: [], supplement: "旧補足", mechanics: [], mechanicsContext: "仕様の説明", themeSupplement: "" },
    { theme: "固定3", tags: [], supplement: "s", mechanics: [mechs[2]], discussionTheme: "  ", mechanicsContext: "ctx", issues: ["q?"] },
  ];
  out.draftToMarkdown = contents.map((c) => ({ content: c, md: pm.paperDraftToMarkdown(c as never) }));
  const fallback = { theme: "FB", tags: ["内部"], supplement: "FB補足", mechanics: mechs, issues: ["FB論点"] };
  const fallbackFixed = { ...fallback, gameTitle: "FBゲーム", mechanicsContext: "FB文脈" };
  const mds = [
    ...out.draftToMarkdown.map((d: { md: string }) => d.md),
    "",
    "自由に書いた本文\nだけ",
    "# 議題\n新議題\n\n# 観点補足\n\n# ゲームのメカニクス\n- **A**: 説明 → 期待感情: 喜び （出所: CRAWL）\n- **B** : x (出所：curated)\n-**C**: bad\n- ** **: 空名\n- **D***: 星\n\n# 論点\n1. 問い1 (両論: ○)\n2) 問い2（両論：賛成側のみ）\n- 問い3 (両論: ×) (補足)\n* 問い4\n3.no\n\n# 議論の経過\n## ラウンド 1\nまとめ",
    "# 議題\r\nCRLF\r\n# 仮説メカニクス (LLM抽出・未検証)\r\n- **H**: h （出所: curated）\r\n",
    "#  \n#\tタブ見出し\n## 小見出し\n#議題\n# 議題   \n本文",
    "# ゲームタイトル（または主目的）\n\n# 議論したいテーマ\nT\n# システム/メカニクスの説明\n文脈\n\n## 抽出されたメカニクス\n- **E**: e （出所: crawl）\n# テーマについての補足情報\n",
    "# 論点\n\n# ゲームのメカニクス\n(メカニクスデータなし)",
    "- **X**: y → 期待感情:   （出所: llm）",
  ];
  out.fromMarkdown = mds.map((md) => ({
    md,
    draft: pm.markdownToPaperDraft(md, fallback as never),
    draftFixed: pm.markdownToPaperDraft(md, fallbackFixed as never),
    fixed: pm.paperFixedFieldsFromMarkdown(md, fallbackFixed as never),
    stripped: pm.stripProgress(md),
  }));
  const rounds = [
    { round: 1, summary: "  まとめ1 ", aufhebung: [] },
    { round: 2, summary: "", aufhebung: ["止揚A", "止揚B"] },
  ];
  out.progress = [
    { base: "# 議題\nX\n\n# 議論の経過\n古い", rounds, conclusion: " 結論 " },
    { base: "# 議題\nX  \n\n", rounds: [], conclusion: "   " },
    { base: "", rounds: [rounds[1]] },
  ].map((c) => ({ ...c, md: pm.renderProgressMarkdown(c.base, c.rounds, c.conclusion) }));

  // ── discussion-paper の純関数 ──
  const persona = (o: Record<string, unknown>) => ({
    id: "p1", name: "論者A", role: "debater", stance: "pro", speechStyle: "丁寧", traits: ["冷静", "皮肉"], model: "m", isLocal: false, ...o,
  });
  const personas = [
    persona({}),
    persona({ id: "p2", name: "意見B", role: "opinion", stance: "opinion", valueAxis: "体験", coreClaims: ["c1"] }),
    persona({ id: "p3", role: "debater", stance: "con", valueAxis: "収益", coreClaims: ["核1", "核2"], possession: { label: "論者#abc", descriptor: "辛口" } }),
    persona({ id: "p4", role: "facilitator", stance: "neutral", isLocal: true, possession: { label: "L" } }),
  ];
  const voices = [
    { content: "あ".repeat(205), source: "steam", sourceUrl: "https://example.invalid/1" },
    { content: "短い声", source: "glab" },
  ];
  const paperBase = { paperId: "P", sessionId: "S", theme: "議題", tags: [] as string[], mechanics: mechs, supplement: "", rounds };
  const ppCases = [
    { paper: paperBase, persona: personas[0], stance: "pro", currentRoundUtterances: [], userVoices: voices },
    { paper: { ...paperBase, tags: ["機密"] }, persona: personas[1], stance: "opinion", currentRoundUtterances: [{ personaName: "A", text: "t" }], userVoices: voices },
    { paper: { ...paperBase, tags: ["機密"] }, persona: personas[3], stance: "neutral", currentRoundUtterances: [], userVoices: [], syntheticOpinions: ["s1"] },
    { paper: { ...paperBase, rounds: [], bodyMd: "# 本文" }, persona: personas[2], stance: "con", currentRoundUtterances: [], userVoices: voices },
    { paper: { ...paperBase, tags: ["開発"], rounds: [], bodyMd: "  " }, persona: personas[1], stance: "weird", currentRoundUtterances: [], userVoices: [] },
  ];
  out.personaPaper = ppCases.map((c) => {
    const p = dp.buildPersonaPaper(c as never);
    return {
      args: c,
      paper: p,
      system: dp.buildPaperSystem(p),
      user: dp.buildPersonaUserPrompt(p, c.stance as never, c.persona as never),
      prompt: dp.paperToPrompt(p, c.stance as never, c.persona as never),
    };
  });
  out.stanceLines = ["neutral", "pro", "con", "opinion", "reform", "conservative", "x"].map((s) => [s, dp.stanceLine(s as never) ?? ""]);
  out.titles = ["【収束】  タイトル\n二行目", "\n\n  \r\n  三行目  ", "", "【収束】", "普通"].map((t) => [t, dp.normalizeDiscussionDisplayTitle(t)]);
  out.synthetic = dp.synthesizeOpinions(mechs as never);

  // ── JSON 抽出 ──
  const raws = [
    '{"a":1}', 'pre {"a":{"b":"}"}} post }', '```json\n{"a":1,}\n{"b":2}\n```', "no json", '{"x": [1,2', "[1,2,3]",
    'x [ {"name":"a"}, {"name":"b", "description":"x\\"}"}, {"name": "cut', '[{"a":1}] trailing ]', "[]", '[ "s", {"bad": }, {"ok":true} ]',
    '{"a":1} and {"b":2}', '"{"',
  ];
  out.extract = raws.map((r) => ({ raw: r, object: ep.extractJsonObject(r), array: me.extractJsonArray(r) }));

  // ── effect-predict / improvement-score ──
  const warns: string[] = [];
  const preds = [
    { changes: [{ dim: "asp.fun", delta: 0.5, reason: "r" }, { dim: "x.unknown", delta: 1 }, { dim: " emo.valence ", delta: 3 }, { dim: "asp.fun", delta: -2 }, { delta: 1 }, "bad"], confidence: "low" },
    { changes: "no" },
    { changes: [], confidence: "HIGH" },
  ];
  out.normalize = preds.map((p) => {
    const w: string[] = [];
    const n = ep.normalizePrediction(p as never, (m) => w.push(m));
    return { raw: p, result: n, vector: n ? ep.changesToVector(n.changes) : null, warns: w };
  });
  const predictCases: Array<{ responses: Resp[]; model?: string }> = [
    { responses: [ok('説明 {"changes":[{"dim":"asp.fun","delta":0.4}],"confidence":"high"}')], model: "small" },
    { responses: [ng("boom")] },
    { responses: [ok("nothing")] },
    { responses: [ok('{"changes":1}')], model: "" },
  ];
  out.predict = [];
  for (const c of predictCases) {
    const f = fakeLlm(c.responses);
    const w: string[] = [];
    const r = await ep.predictEffect({ opinion: "意見", theme: "テーマ", mechanics: [...mechs, ...mechs, ...mechs] as never, llm: f.llm as never, model: c.model, warn: (m) => w.push(m) });
    (out.predict as unknown[]).push({ ...c, result: r, calls: f.calls, warns: w });
  }
  const gap = Array.from({ length: 20 }, (_, i) => (i % 3) - 1);
  const mv = (k: number) => Array.from({ length: 20 }, (_, i) => Math.sin(i + k));
  const scored = imp.scoreOpinions({
    candidates: [
      { id: "a", movement: mv(1), method: "effect-predict", detail: [{ dim: "asp.fun", delta: 0.1 }] },
      { id: "b", movement: mv(2), method: "lexicon-fallback" },
      { id: "c", movement: mv(1), method: "effect-predict" },
    ],
    designGap: gap,
  });
  out.improvement = { gap, scored, winner: imp.pickWinner(scored), empty: imp.pickWinner([]), opinion: imp.scoreOpinion(mv(3), mv(4), gap) };

  // ── mechanic-extract ──
  const enrichCases = [
    { existing: mechs, target: 3, responses: [ok("[]")] },
    { existing: mechs.slice(0, 1), target: 5, responses: [ok('[{"name":" ガチャ "},{"name":"新規","description":" d ","intended_affect":" "},{"name":""},{"name":"新規2","intended_affect":"楽"},{"name":"新規3"},{"name":"新規4"}]')] },
    { existing: [], target: 2, responses: [ng("down")] },
    { existing: [], target: 200, responses: [ok("broken")] },
  ];
  out.enrich = [];
  for (const c of enrichCases) {
    const f = fakeLlm(c.responses);
    const w: string[] = [];
    const r = await me.enrichMechanics({ theme: "テーマ", existing: c.existing as never, voices: [{ content: "い".repeat(170), source: "s" }, ...Array.from({ length: 30 }, (_, i) => ({ content: `声${i}` }))], llm: f.llm as never, target: c.target, model: "mm", warn: (m) => w.push(m) });
    (out.enrich as unknown[]).push({ ...c, result: r, calls: f.calls, warns: w });
  }

  // ── spec-analyze ──
  const longSpec = "仕様😀".repeat(12000);
  const specCases = [
    { specText: "  ", responses: [ok("[]")] },
    { specText: "ガチャの仕様", maxMechanics: 2, responses: [ok('[{"name":"A","description":" d ","intended_valence":" Positive ","intended_aspects":[" x ", "", 1],"intended_emotions":[]},{"name":"a"},{"name":"B"},{"name":"C"}]')] },
    { specText: longSpec, responses: [ok('[{"name":"X"}]'), ng("e2"), ok("bad"), ok('[{"name":"x"},{"name":"Y"}]')] },
  ];
  out.spec = [];
  for (const c of specCases) {
    const f = fakeLlm(c.responses);
    const w: string[] = [];
    const r = await sa.analyzeSpecMechanics({ theme: "T", specText: c.specText, llm: f.llm as never, maxMechanics: c.maxMechanics, warn: (m) => w.push(m) });
    (out.spec as unknown[]).push({ ...c, specText: c.specText === longSpec ? "LONG" : c.specText, result: r, calls: f.calls.map((x: any) => ({ ...x, prompt: x.prompt.length > 5000 ? `LEN:${x.prompt.length}` : x.prompt })), warns: w });
  }

  // ── vote (純関数) ──
  out.voteParse = [
    ["id-1", ["id-1", "id-2"], []], ["選んだのは id-2 です", ["id-1", "id-2"], []], ["2", ["a", "b"], ["b", "a"]], [" 1。 ", ["a", "b"], ["b", "a"]],
    ["3番", ["a", "b"], ["b", "a"]], ["1234", ["a"], ["a"]], ["0", ["a"], ["a"]], ["2.", ["a", "b"], ["a", "b"]], ["x", ["a"], ["a"]],
  ].map(([t, v, p]) => ({ text: t, valid: v, presented: p, id: vote.parseVoteId(t as string, new Set(v as string[]), p as string[]) }));
  out.voteWinner = [
    [{ a: 1, b: 2 }, 0.5], [{ b: 2, a: 2, c: 1 }, 0.99], [{ b: 2, a: 2 }, 0], [{}, 0.5], [{ x: 0 }, 0.5],
  ].map(([t, r]) => ({ tally: t, r, result: vote.pickVoteWinner(t as never, () => r as number) }));

  // ── voice-cache / mix / paper-voices の検索 ──
  out.voiceKeys = [[["B", "a", " Ａ ", "", "😀", "ｚ"], 5], [[], 0]];
  const store = [
    { content: "ガチャが楽しい", source: "s" }, { content: "周回がつらい", source: "s" }, { content: "ガチャと周回", source: "s2", sourceUrl: "u" },
  ];
  out.storedLookup = [[["ガチャ"], 2], [["周回 ガチャ"], 5], [["無関係"], 1], [["ガチャ"], 0]].map(([t, l]) => ({ terms: t, limit: l, result: pv.storedVoiceLookup(store)(t as string[], l as number) }));
  out.mix = mix.makeMixedVoiceLookup((terms, n) => [store, [store[1], { content: " 周回がつらい ", source: "x" }, { content: "別", source: "y" }], []])(["x"], 4);
  out.mixEvenly = mix.mixEvenly([[1, 2, 3], [1, 4], [5]], 10, (v) => String(v));
  const cache = vc.createVoiceCache((terms, limit) => [{ content: terms.join("+") + limit, source: "s" }]);
  out.cache = [cache.lookup(["A", "b"], 3), cache.lookup(["b ", "a"], 3), cache.lookup(["a"], 3), cache.misses(), cache.hits()];

  // ── paper-reuse の番号 ──
  out.reuseNo = ["ペーパー#12", "PAPER ＃ ３４ です", "paper12", "ペーパー 0", "ペーパー x paper 7", "なし", "", "ペーパー#000123", "ペーパー＃"].map((t) => [t, reuse.parseReusePaperNo(t)]);

  // ── 議論適性 ──
  const debVoices = [
    { content: "とても楽しい最高", source: "steam" }, { content: "つまらない最悪", source: "steam" }, { content: "普通", source: " " }, { content: "楽しい", source: undefined },
  ];
  out.evidence = deb.computeEvidenceBalance(debVoices);
  out.evidenceEmpty = deb.computeEvidenceBalance([]);
  const debCases: Array<{ responses: Resp[]; min: number }> = [
    { responses: [ok('["論点1","論点2", 3, " "]'), ok('[{"index":1,"armable":"both"},{"index":"2","armable":"pro-only"},{"index":9,"armable":"both"}]')], min: 2 },
    { responses: [ng("x")], min: 2 },
    { responses: [ok('["論点1"]'), ok("[]")], min: 1 },
    { responses: [ok("[]"), ok("[]")], min: 2 },
    { responses: [ok('["A","B"]'), ok('[{"index":1.7,"armable":"neither"},{"index":2,"armable":"bogus"}]')], min: 3 },
  ];
  out.debatability = [];
  for (const c of debCases) {
    const f = fakeLlm(c.responses);
    const w: string[] = [];
    const r = await deb.assessDebatability({ theme: "議題", paperMd: "# 議題\nX", voices: debVoices, llm: f.llm as never, minArmableIssues: c.min, model: "jm", toVector: (t) => [t.includes("楽しい") ? 0.9 : t.includes("最悪") ? 0.1 : 0.5], warn: (m) => w.push(m) });
    (out.debatability as unknown[]).push({ ...c, result: r, annotated: deb.annotatedIssues(r), calls: f.calls, warns: w });
  }

  // ── dialectic の純関数 ──
  const issue = { id: "I1", sessionId: "S", title: "ガチャは必要か", ordinal: 1, source: "llm", status: "open" };
  const posA = { id: "PA", issueId: "I1", personaId: "p1", stance: "pro", claim: "必要", qualifier: "ライト層", grounds: [
    { id: "pro-G1", text: "売上", state: "unchallenged", kind: "induction", warrant: "収益源" },
    { id: "pro-G2", text: "期待感", state: "conceded" },
    { id: "pro-G3", text: "継続", state: "defended", kind: "deduction" },
  ], values: ["収益"] };
  const posB = { id: "PB", issueId: "I1", personaId: "p3", stance: "con", claim: "不要", qualifier: null, grounds: [
    { id: "con-G1", text: "射幸心", state: "challenged" },
  ], values: [] };
  const posBdown = { ...posB, grounds: [{ id: "con-G1", text: "x", state: "conceded" }] };
  const posAdown = { ...posA, grounds: [{ id: "pro-G1", text: "x", state: "challenged" }] };
  out.goal = [[posA, posB], [posAdown, posB], [posA, posBdown], [posAdown, posBdown]].map(([a, b]) => ({
    standingA: goal.standingGroundIds(a as never), result: goal.detectRefutation(a as never, b as never),
  }));
  const tensions = ["open", "resolved", "synthesized", "compromised", "agreed_disagree", "unresolved_fact", "agreed", "aligned", "partially_aligned"].map((s, i) => ({
    id: `T${i}`, issueId: "I1", positionA: "PA", positionB: "PB", type: "values", status: s, resolutionNote: i % 2 ? "メモ" : null,
  }));
  const synthRec = { id: "Y", tensionId: "T", preserves: [], negates: [], elevates: null, kind: "compromise", text: "折衷文", ratification: [], status: "rejected", revision: 0 };
  out.outcome = {
    reasons: tensions.map((t) => [oc.noConclusionReason(t as never), oc.noConclusionReason(t as never, synthRec as never)]),
    outcomes: [oc.issueOutcome({ refutation: "x", tensions: tensions as never }), oc.issueOutcome({ refutation: null, tensions: tensions as never }), oc.issueOutcome({ refutation: null, tensions: [tensions[0]] as never })],
    line: oc.renderOutcomeLine("aufheben", "詳細"),
  };
  out.stance = {
    prompt: [pst.buildStancePrompt(issue as never, "主張", null), pst.buildStancePrompt(issue as never, "主張", "限定")],
    parse: ["Yes", "no, yes", "UNCLEAR yes", "nothing", "none"].map((t) => pst.parseStanceAnswer(t)),
    reversed: [pst.isReversed("pro", "no"), pst.isReversed("con", "no"), pst.isReversed("pro", "unclear")],
    feedback: [pst.stanceFeedback("pro", "前"), pst.stanceFeedback("con", "前")],
  };
  out.issueVoices = [iv.renderIssueVoices([]), iv.renderIssueVoices([{ content: "  a\n\nb  ", source: "s" }, { content: "😀".repeat(100), source: "t" }])];
  out.positionPrompt = [
    pos.buildPositionPrompt({ persona: personas[2] as never, stance: "con", issue: issue as never, voicesBlock: "VOICES", feedback: "FB" }),
    pos.buildPositionPrompt({ persona: personas[0] as never, stance: "pro", issue: issue as never }),
  ];
  out.ruleCheck = {
    anchors: [
      { attack: "data", groundId: "pro-G1", data: "d", warrant: "w" }, { attack: "data", groundId: "pro-G2", data: "d", warrant: "w" },
      { attack: "qualifier", groundId: null, data: "d", warrant: "w" }, { attack: null, groundId: "pro-G1", data: "d", warrant: "w" },
      { attack: "warrant", groundId: "pro-G1", data: null, warrant: "w" }, { attack: "warrant", groundId: "zz", data: "d", warrant: "w" },
    ].map((c) => ({ claim: c, result: rc.checkRebutAnchor({ ...c, position: posA } as never) })),
    judgePrompt: [
      rc.buildRuleJudgePrompt({ issue, position: posA, ownPosition: posB, ground: posA.grounds[0], attack: "data", rebutText: "反論", rebutData: "rd", rebutWarrant: "rw" } as never),
      rc.buildRuleJudgePrompt({ issue, position: posB, ground: null, attack: "qualifier", rebutText: "反論", rebutData: "rd", rebutWarrant: "rw" } as never),
    ],
    verdicts: ["OK", "straw_man だけど ok", "topic_shift", "none", "ok dismissal"].map((t) => rc.parseRuleVerdict(t)),
    concessionPrompt: rc.buildConcessionPrompt("相手", "返答"),
    notices: ["incomplete", "quote_mismatch", "dismissal"].map((f) => rc.renderFoulNotice("A", f as never)),
  };
  out.tensionPure = {
    classify: ten.buildClassifyPrompt(issue as never, posA as never, posB as never),
    types: ["Means and values", "TRADEOFF", "x", "facts"].map((t) => ten.parseTensionType(t)),
    facts: ["A", "b です", "None", "どちらも", "answer is B", "i think a, not b", "(a)", "abc", "", "A1", "a_b"].map((t) => [t, ten.parseFactVerdict(t)]),
    factPrompt: ten.buildFactResolvePrompt(issue as never, posA as never, posB as never, [{ content: "x".repeat(250), source: "s" }]),
  };
  out.synthPure = {
    templates: ["values", "means", "tradeoff", "facts"].map((t) => syn.synthesisTemplateFor(t as never)),
    prompt: [
      syn.buildSynthesisPrompt({ issueTitle: "論点", tension: tensions[0] as never, positionA: posA as never, positionB: posB as never }),
      syn.buildSynthesisPrompt({ issueTitle: "論点", tension: { ...tensions[0], type: "means" } as never, positionA: posA as never, positionB: posB as never, missingFeedback: ["pro-G1"] }),
    ],
    ratify: syn.buildRatifyPrompt({ position: posA as never, personaName: "A", synthesisText: "統合", preserves: [], elevates: "枠" }),
  };
  const utterances = new Map([["u1", { id: "u1", personaName: "A", text: "主張1".repeat(50) }], ["u2", { id: "u2", personaName: "B", text: "反論" }]]);
  const names = new Map([["p1", "論者A"], ["p3", "論者C"]]);
  out.turnPrompt = [
    tp.buildDialecticTurnPrompt({ persona: personas[0] as never, stance: "pro", issue: issue as never, positions: [posA, posB] as never, unresolvedRebuttals: [{ rebutId: "u2", targetId: "u1", byPersonaId: "p3", targetPersonaId: "p1", state: "challenged" }, { rebutId: "zz", targetId: "yy", byPersonaId: "p3", state: "challenged" }] as never, utterances, personaNames: names, allowedActs: ["support", "rebut"], targetUtteranceId: "u2", nomination: "どうぞ", voicesBlock: "VB" }),
    tp.buildDialecticTurnPrompt({ persona: personas[1] as never, stance: "opinion", issue: issue as never, positions: [], unresolvedRebuttals: [], utterances, personaNames: names, allowedActs: ["claim"] }),
  ];
  const turnRaws = [
    '{"act":"rebut","target":"u1","attack":"データ","groundId":" pro-G1 ","data":" d ","warrant":"w","text":" 本文 "}',
    '{"act":"synthesize","target":"zz","text":"t"}', '{"act":"support","text":"t"}', "壊れた応答", '{"text":"  "}', '{"act":"question","target":"u2","text":"q","attack":"bogus"}',
  ];
  out.parseTurn = turnRaws.map((r) => {
    const w: string[] = [];
    return { raw: r, result: tp.parseTurnResponse(r, { allowedActs: ["rebut", "question", "support"], validTargets: new Set(["u1", "u2"]), defaultTargetId: "u2", warn: (m) => w.push(m) }), warns: w };
  });
  // 応酬のシミュレーション
  const exch = new ex.GroundExchange({ maxDepth: 3 });
  const nn = (side: string, personaId: string, data: string, groundId: string | null = null) => ({ utteranceId: `u-${data}`, side, personaId, groundId, data, text: `say ${data}` }) as never;
  const o1 = exch.addOpening(nn("pro", "p1", "g1", "pro-G1"));
  const o2 = exch.addOpening(nn("pro", "p1", "g2", "pro-G2"));
  const o3 = exch.addOpening(nn("con", "p3", "g3", "con-G1"));
  const log: unknown[] = [];
  log.push(exch.next());
  const r1 = exch.rebut(o1.id, nn("con", "p3", "r1"));
  exch.accept(o2.id);
  log.push(exch.next());
  const r2 = exch.rebut(o3.id, nn("pro", "p1", "r2"));
  log.push(exch.next());
  exch.fail(r1.id);
  const r3 = exch.rebut(r2.id, nn("con", "p3", "r3"));
  const r4 = exch.rebut(r3.id, nn("pro", "p1", "r4"));
  log.push(exch.next(), exch.cutRemaining(), exch.settled());
  out.exchange = {
    log, all: exch.all(), path: exch.path(r4.id), children: exch.children(o3.id),
    verdicts: exch.all().map((n) => [n.id, exch.verdict(n.id)]), groundStates: [...exch.groundStates()],
    standing: [exch.standingCount("pro"), exch.standingCount("con")], opposing: ex.opposingSide("pro"),
    prompt: xp.buildExchangePrompt({ persona: personas[2] as never, stance: "con", issue: issue as never, positions: [posA, posB] as never, personaNames: names, path: exch.path(r3.id), voicesBlock: "VB" }),
    prompt2: xp.buildExchangePrompt({ persona: personas[0] as never, stance: "opinion", issue: issue as never, positions: [], personaNames: new Map(), path: [] }),
  };
  out.parseExchange = ['{"act":"Concede","text":"t"}', '{"act":"x","attack":"warrant","kind":"帰納","data":"d","warrant":"w","text":"t","claimKind":"sourceQuote","evidenceIds":["E1","[E2]","E1"]}', "bad"].map((r) => {
    const w: string[] = [];
    return { raw: r, result: xp.parseExchangeResponse(r, (m) => w.push(m)), warns: w };
  });
  // スケジューラ
  const g = new ArgumentGraph();
  g.apply({ id: "c1", personaId: "p1", act: "claim", targetId: null, turn: 1 });
  g.apply({ id: "c2", personaId: "p3", act: "claim", targetId: null, turn: 2 });
  g.apply({ id: "r1", personaId: "p3", act: "rebut", targetId: "c1", turn: 3 });
  const speakers = [personas[0], personas[2], persona({ id: "p5", stance: "con" })];
  const anchors = [{ personaId: "p1", stance: "pro", utteranceId: "c1" }, { personaId: "p3", stance: "con", utteranceId: "c2" }];
  const counts = new Map([["p1", 2], ["p3", 1], ["p5", 1]]);
  const d1 = sch.scheduleNextTurn({ graph: g, speakers: speakers as never, positionAnchors: anchors as never, speakCounts: counts });
  const d2 = sch.scheduleNextTurn({ graph: g, speakers: speakers.slice(1) as never, positionAnchors: anchors as never, speakCounts: counts });
  const d3 = sch.scheduleNextTurn({ graph: new ArgumentGraph(), speakers: [personas[1]] as never, positionAnchors: [], speakCounts: new Map() });
  const d4 = sch.scheduleNextTurn({ graph: g, speakers: [], positionAnchors: [], speakCounts: counts });
  const tgt = { id: "u1", personaName: "B", text: "長い".repeat(40) };
  out.scheduler = {
    decisions: [d1, d2, d3, d4],
    fallback: [d1, d2, d3].flatMap((d) => [sch.nominationFallbackText(d!, "A", tgt), sch.nominationFallbackText(d!, "A")]),
    frame: [d1, d2, d3].flatMap((d) => [sch.nominationFrameText(d!, "A", tgt), sch.nominationFrameText(d!, "A")]),
    quotes: ["短い", "あ".repeat(170), "一文目です。" + "い".repeat(160), "a。" + "b".repeat(200), " a\n b ", "😀".repeat(100)].map((t) => sch.quoteExcerpt(t)),
  };

  // ── dialectic の LLM 経路 (純粋: DB 不使用) ──
  const llmCase = async (responses: Resp[], fn: (llm: never, warn: (m: string) => void) => Promise<unknown>) => {
    const f = fakeLlm(responses);
    const w: string[] = [];
    let result: unknown;
    let error: string | null = null;
    try {
      result = await fn(f.llm as never, (m) => w.push(m));
    } catch (e) {
      error = (e as Error).message;
    }
    return { responses, result: result ?? null, error, calls: f.calls, warns: w };
  };
  out.llmDialectic = {
    decompose: [
      await llmCase([ok('["Q1"," Q2 ",1,"","Q3","Q4","Q5","Q6"]')], (llm) => issues.decomposeIssues({ theme: "T", paperMd: " # 本文 ", llm, model: "m" })),
      await llmCase([ok('["Q1","Q2"]')], (llm) => issues.decomposeIssues({ theme: "T", llm, maxIssues: 0 })),
      await llmCase([ng("e")], (llm) => issues.decomposeIssues({ theme: "T", llm })),
      await llmCase([ok("x")], (llm) => issues.decomposeIssues({ theme: "T", llm })),
    ],
    position: [
      await llmCase([ok('{"claim":" 主張 ","qualifier":" ","grounds":[" 旧形式 ",{"kind":"帰納","data":"D","warrant":"W","say":"S","claimKind":"sourceQuote","evidenceIds":["E1"]},{"text":"T2","warrant":"W2"},{"data":" "},{"data":"D4"}],"values":["v1"," ","v2","v3","v4","v5"],"text":""}')],
        (llm, warn) => pos.generatePosition({ theme: "T", issue: issue as never, persona: personas[2] as never, stance: "con", paperSystem: "SYS", llm, model: "mm", warn })),
      await llmCase([ok('{"claim":"c","grounds":[]}')], (llm, warn) => pos.generatePosition({ theme: "T", issue: issue as never, persona: personas[0] as never, stance: "pro", paperSystem: "SYS", llm, warn })),
      await llmCase([ng("down")], (llm, warn) => pos.generatePosition({ theme: "T", issue: issue as never, persona: personas[2] as never, stance: "con", paperSystem: "SYS", llm, warn })),
      await llmCase([ng("down")], (llm, warn) => pos.generatePosition({ theme: "T", issue: issue as never, persona: personas[1] as never, stance: "pro", paperSystem: "SYS", llm, warn })),
      await llmCase([ok("  ただの文章" + "x".repeat(250))], (llm, warn) => pos.generatePosition({ theme: "T", issue: issue as never, persona: personas[0] as never, stance: "pro", paperSystem: "SYS", llm, warn })),
      await llmCase([ok("")], (llm, warn) => pos.generatePosition({ theme: "T", issue: issue as never, persona: personas[0] as never, stance: "pro", paperSystem: "SYS", llm, warn })),
    ],
    stance: [
      await llmCase([ok("No")], (llm, warn) => pst.judgePositionStance({ issue: issue as never, claim: "c", qualifier: null, llm, model: "j", warn })),
      await llmCase([ng("e")], (llm, warn) => pst.judgePositionStance({ issue: issue as never, claim: "c", qualifier: "q", llm, warn })),
    ],
    gate: [
      await llmCase([ok("straw_man")], (llm, warn) => rc.gateRebut({ attack: "data", groundId: "pro-G1", data: "d", warrant: "w", issue: issue as never, position: posA as never, ownPosition: posB as never, rebutText: "t", judge: true, llm, model: "j", warn })),
      await llmCase([ok("???")], (llm, warn) => rc.gateRebut({ attack: "qualifier", groundId: null, data: "d", warrant: "w", issue: issue as never, position: posA as never, rebutText: "t", judge: true, llm, warn })),
      await llmCase([ng("e")], (llm, warn) => rc.gateRebut({ attack: "data", groundId: "pro-G1", data: "d", warrant: "w", issue: issue as never, position: posA as never, rebutText: "t", judge: true, llm, warn })),
      await llmCase([ok("x")], (llm, warn) => rc.gateRebut({ attack: "data", groundId: "pro-G1", data: "d", warrant: "w", issue: issue as never, position: posA as never, rebutText: "t", judge: false, llm, warn })),
      await llmCase([ok("x")], (llm, warn) => rc.gateRebut({ attack: "data", groundId: "zz", data: "d", warrant: "w", issue: issue as never, position: posA as never, rebutText: "t", judge: true, llm, warn })),
    ],
    concession: [
      await llmCase([ok("REBUT then concede")], (llm, warn) => rc.judgeConcession({ targetText: "a", replyText: "b", llm, model: "j", warn })),
      await llmCase([ok("concede")], (llm, warn) => rc.judgeConcession({ targetText: "a", replyText: "b", llm, warn })),
      await llmCase([ok("?")], (llm, warn) => rc.judgeConcession({ targetText: "a", replyText: "b", llm, warn })),
      await llmCase([ng("e")], (llm, warn) => rc.judgeConcession({ targetText: "a", replyText: "b", llm, warn })),
    ],
    classify: [
      await llmCase([ok("tradeoff")], (llm, warn) => ten.classifyTension({ issue: issue as never, positionA: posA as never, positionB: posB as never, llm, model: "j", warn })),
      await llmCase([ok("??")], (llm, warn) => ten.classifyTension({ issue: issue as never, positionA: posA as never, positionB: posB as never, llm, warn })),
      await llmCase([ng("e")], (llm, warn) => ten.classifyTension({ issue: issue as never, positionA: posA as never, positionB: posB as never, llm, warn })),
    ],
    fact: [
      await llmCase([ok("B")], (llm, warn) => ten.resolveFactTension({ theme: "T", issue: issue as never, positionA: posA as never, positionB: posB as never, listExternalVoices: (t, n) => [{ content: t.join("|") + n, source: "s" }], llm, warn })),
      await llmCase([ok("none")], (llm, warn) => ten.resolveFactTension({ theme: "T", issue: issue as never, positionA: posA as never, positionB: posB as never, listExternalVoices: () => [{ content: "c", source: "s" }], llm, warn })),
      await llmCase([ng("e")], (llm, warn) => ten.resolveFactTension({ theme: "T", issue: issue as never, positionA: posA as never, positionB: posB as never, listExternalVoices: () => [{ content: "c", source: "s" }], llm, warn })),
      await llmCase([ok("A")], (llm, warn) => ten.resolveFactTension({ theme: "T", issue: issue as never, positionA: posA as never, positionB: posB as never, listExternalVoices: () => { throw new Error("kg down"); }, llm, warn })),
    ],
    elevation: [
      await llmCase([ok("x")], (llm, warn) => syn.elevationGate({ elevates: "  ", llm, warn })),
      await llmCase([ok("x")], (llm, warn) => syn.elevationGate({ elevates: "両方やる", llm, warn })),
      await llmCase([ok("Aufheben だが compromise")], (llm, warn) => syn.elevationGate({ elevates: "Both の上位目標を再定義して条件分割で両立させる長い説明", llm, model: "j", warn })),
      await llmCase([ok("compromise")], (llm, warn) => syn.elevationGate({ elevates: "新しい枠の説明がここにあり十分な長さを持つ文", llm, warn })),
      await llmCase([ok("?")], (llm, warn) => syn.elevationGate({ elevates: "新しい枠の説明がここにあり十分な長さを持つ文", llm, warn })),
      await llmCase([ng("e")], (llm, warn) => syn.elevationGate({ elevates: "新しい枠の説明がここにあり十分な長さを持つ文", llm, warn })),
    ],
    ratify: [
      await llmCase([ok('{"verdict":"accept","preservedIds":[" pro-G1 ","pro-G3","con-G1"],"missing":["pro-G2","x"],"reason":"r"}')], (llm, warn) => syn.ratifySynthesis({ position: posA as never, personaName: "A", synthesisText: "s", preserves: ["pro-G1"], elevates: null, llm, warn })),
      await llmCase([ok('{"verdict":"accept","preservedIds":["pro-G1"]}')], (llm, warn) => syn.ratifySynthesis({ position: posA as never, personaName: "A", synthesisText: "s", preserves: [], elevates: "e", llm, model: "j", warn })),
      await llmCase([ok("bad")], (llm, warn) => syn.ratifySynthesis({ position: posA as never, personaName: "A", synthesisText: "s", preserves: [], elevates: null, llm, warn })),
      await llmCase([ng("e")], (llm, warn) => syn.ratifySynthesis({ position: posB as never, personaName: "C", synthesisText: "s", preserves: [], elevates: null, llm, warn })),
    ],
    generate: [
      await llmCase([ok('{"text":" 統合 ","preserves":["pro-G1","zz"," con-G1 "],"negates":["pro-G2"],"elevates":" 枠 "}')], (llm, warn) => syn.generateSynthesis({ issueTitle: "I", tension: tensions[0] as never, positionA: posA as never, positionB: posB as never, paperSystem: "SYS", llm, model: "mm", warn })),
      await llmCase([ok(" 生の文 ")], (llm, warn) => syn.generateSynthesis({ issueTitle: "I", tension: tensions[0] as never, positionA: posA as never, positionB: posB as never, paperSystem: "SYS", llm, warn })),
      await llmCase([ng("e")], (llm, warn) => syn.generateSynthesis({ issueTitle: "I", tension: tensions[0] as never, positionA: posA as never, positionB: posB as never, paperSystem: "SYS", llm, warn })),
    ],
    nomination: [
      await llmCase([ok("  ")], (llm, warn) => sch.renderNomination({ decision: d1!, personaName: "A", target: tgt, llm, model: "f", warn })),
      await llmCase([ng("e")], (llm, warn) => sch.renderNomination({ decision: d3!, personaName: "A", llm, warn })),
      await llmCase([ok(" 進行文 ")], (llm, warn) => sch.renderFacilitatorLine({ content: "内容", llm, warn })),
    ],
  };

  out.inputs = { issue, posA, posB, posAdown, posBdown, tensions, synthRec, personas, names: Object.fromEntries(names), tgt, utterances: [...utterances.values()], speakers, anchors, counts: Object.fromEntries(counts), mechs };

  // ── 代理の定立の帰属 ──
  const proxyPersona = persona({ proxy: { label: "論者#a", speakerKey: "k", genre: "g", bundleText: "b", evidenceIds: ["E1", "E2"], evidenceTexts: { E1: "このゲームは最高に楽しい", E2: "課金が高すぎる" } } });
  const gp = {
    claim: "c", qualifier: null, values: [], text: "t", degraded: false, groundTexts: ["g1", "g2", "g3"],
    grounds: [
      { id: "pro-G1", text: "「最高に楽しい」と言っている", state: "unchallenged", claimKind: "sourceQuote", evidenceIds: ["E1"] },
      { id: "pro-G2", text: "「安い」と言った", state: "unchallenged", claimKind: "sourceQuote", evidenceIds: ["E2"] },
      { id: "pro-G3", text: "推測", state: "unchallenged" },
    ],
  };
  const paw: string[] = [];
  out.positionAttribution = { input: gp, persona: proxyPersona, result: pa.attributePositionGrounds(gp as never, proxyPersona as never, (m) => paw.push(m)), warns: paw, plain: pa.attributePositionGrounds(gp as never, personas[0] as never) };

  // ── DB を使う経路 (呼び出しと戻り値だけを記録。id / 時刻は記録しない) ──
  const f1 = fakeLlm([ok("[]"), ok("[]")]);
  const w1: string[] = [];
  const voteResults = [] as unknown[];
  const lcg = (() => { let s = 7; return () => { s = (s * 1664525 + 1013904223) % 4294967296; return s / 4294967296; }; })();
  const fv = fakeLlm([ok("u2"), ok("2"), ng("e"), ok("??"), ok("[ID:u1] u1")]);
  voteResults.push(await vote.runRoundVote({ theme: "T", sessionId: "VS", round: 1, voterCount: 5, utterances: [{ id: "u1", personaName: "A", text: "t1" }, { id: "u2", personaName: "B", text: "t2" }, { id: "u3", personaName: "C", text: "t3" }], llm: fv.llm as never, rng: lcg, warn: (m) => w1.push(m) }));
  voteResults.push(await vote.runRoundVote({ theme: "T", sessionId: "VS", round: 2, voterCount: 3, utterances: [], llm: f1.llm as never }));
  out.voteRun = { results: voteResults, calls: fv.calls, warns: w1 };

  const fs1 = fakeLlm([ok('前置き [{"personaId":"p1","valueAxis":" 体験 ","coreClaims":[" a ","", "b", "c"]},{"personaId":"p2","valueAxis":"","coreClaims":["x"]},{"personaId":"p3"},{"personaId":"p1","valueAxis":"後勝ち"}]')]);
  const ws1: string[] = [];
  const setupA = await ps.setupSessionPersonas({ theme: "T", sessionId: "PS", personas: personas as never, llm: fs1.llm as never, model: "m", warn: (m) => ws1.push(m) });
  const fs2 = fakeLlm([ok("[broken")]);
  const setupB = await ps.setupSessionPersonas({ theme: "T", sessionId: "PS2", personas: personas.slice(0, 1) as never, llm: fs2.llm as never, warn: (m) => ws1.push(m) });
  const fs3 = fakeLlm([ng("down")]);
  const setupC = await ps.setupSessionPersonas({ theme: "T", sessionId: "PS3", personas: personas.slice(0, 1) as never, llm: fs3.llm as never, warn: (m) => ws1.push(m) });
  out.personaSetup = { results: [setupA, setupB, setupC], calls: [fs1.calls, fs2.calls, fs3.calls], warns: ws1.map((m) => m.replace(/JSON パース失敗: .* — /, "JSON パース失敗: <msg> — ")) };

  const rsw: string[] = [];
  const fr1 = fakeLlm([ok("  まとめ文  "), ok('{"found": true, "summary": "止揚X"}')]);
  const sumA = await rs.summarizeRound({ theme: "T", sessionId: "RS", paperId: "PP", round: 1, utterances: [{ personaName: "A", text: "a" }, { personaName: "B", text: "b" }], stockedAufhebung: ["前の止揚"], llm: fr1.llm as never, warn: (m) => rsw.push(m) });
  const fr2 = fakeLlm([ng("e1"), ok("not json")]);
  const sumB = await rs.summarizeRound({ theme: "T", sessionId: "RS", paperId: "PP", round: 2, utterances: [{ personaName: "A", text: "a" }, { personaName: "B", text: "b" }], stockedAufhebung: [], llm: fr2.llm as never, warn: (m) => rsw.push(m) });
  const fr3 = fakeLlm([ok("   ")]);
  const sumC = await rs.summarizeRound({ theme: "T", sessionId: "RS", paperId: "PP", round: 3, utterances: [{ personaName: "A", text: "a" }], stockedAufhebung: [], llm: fr3.llm as never, warn: (m) => rsw.push(m) });
  out.roundSummary = { results: [sumA, sumB, sumC], calls: [fr1.calls, fr2.calls, fr3.calls], warns: rsw };

  const rfw: string[] = [];
  const refineOut = [];
  for (const r of [ok("  # 議題\n改訂  "), ok("議題なし"), ng("e")]) {
    const f = fakeLlm([r]);
    refineOut.push({ result: await pr.refinePaperBrief({ baseBodyMd: r.ok && r.text.includes("改訂") ? "  # 議題\nX " : "", theme: "T", rounds, conclusion: " 結論 ", llm: f.llm as never, sessionId: "RF", model: "m", warn: (m) => rfw.push(m) }), calls: f.calls });
  }
  out.refine = { results: refineOut, warns: rfw };

  const synW: string[] = [];
  const fgen = fakeLlm([ok('{"text":"案1","preserves":["pro-G1"],"negates":[],"elevates":"両方"}'), ok('{"text":"案2","preserves":["pro-G1","pro-G3","con-G1"],"negates":[],"elevates":"新しい上位目標を導入して条件で分割する案"}')]);
  const fgate = fakeLlm([ok("aufheben")]);
  const frat = fakeLlm([ok('{"verdict":"reject","preservedIds":[],"missing":["pro-G2"]}'), ok('{"verdict":"accept","preservedIds":["con-G1"]}'), ok('{"verdict":"accept","preservedIds":["pro-G1","pro-G3"]}'), ok('{"verdict":"accept","preservedIds":["con-G1"]}')]);
  const loopA = await syn.runSynthesisLoop({ issueTitle: "I", tension: tensions[0] as never, positionA: posA as never, positionB: posB as never, personaNames: names, paperSystem: "SYS", genLlm: fgen.llm as never, gateLlm: fgate.llm as never, ratifyLlm: frat.llm as never, mainModel: "main", judgeModel: "judge", warn: (m) => synW.push(m) });
  const fgen2 = fakeLlm([ng("e")]);
  const fgate2 = fakeLlm([ok("x")]);
  const frat2 = fakeLlm([ok("bad")]);
  const loopB = await syn.runSynthesisLoop({ issueTitle: "I", tension: tensions[0] as never, positionA: posA as never, positionB: posB as never, personaNames: new Map(), paperSystem: "SYS", genLlm: fgen2.llm as never, gateLlm: fgate2.llm as never, ratifyLlm: frat2.llm as never, warn: (m) => synW.push(m) });
  const strip = (r: { record: Record<string, unknown> }) => ({ ...r, record: { ...r.record, id: "<id>" } });
  out.synthesisLoop = { results: [strip(loopA as never), strip(loopB as never)], calls: [fgen.calls, fgate.calls, frat.calls, fgen2.calls, fgate2.calls, frat2.calls], warns: synW };

  const dir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "flow");
  fs.writeFileSync(path.join(dir, "w2g_golden.json"), JSON.stringify(out, null, 2) + "\n");
  fs.rmSync(tmp, { recursive: true, force: true });
  console.log("wrote w2g_golden.json");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
