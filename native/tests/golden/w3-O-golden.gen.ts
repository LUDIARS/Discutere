// w3-O 担当 (ペーパーレビュー・各種 API ルート・軽量 Web チャットの部品) の Node 実装の入出力を記録する。
// 実行: npx tsx native/tests/golden/w3-O-golden.gen.ts
//   → native/tests/fixtures/w3o/golden.json と画面 native/web/api/{personas,noise}.html
// LLM は決まった応答を順に返す偽物 (呼び出しも記録する)。外部サービスには繋がない。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "w3o-golden-"));
process.env.DATABASE_PATH = path.join(tmp, "flow.db");
process.env.DISCUTERE_CONFIG = path.join(tmp, "no-config.json");

type Resp = { ok: true; text: string } | { ok: false; error: string } | { throws: string };
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
        if ("throws" in r) throw new Error(r.throws);
        return r.ok ? { ok: true as const, text: r.text } : { ok: false as const, error: r.error };
      },
    },
  };
}
const ok = (text: string): Resp => ({ ok: true, text });
const ng = (error: string): Resp => ({ ok: false, error });
const boom = (message: string): Resp => ({ throws: message });

const GAME_MD = [
  "---",
  "id: monster-strike",
  "title: モンスターストライク",
  "mechanics:",
  "  - name: ギミック",
  "    description: クエストごとに妨害が置かれる",
  "  - name: アビリティ",
  "    description: ギミックに対応して編成を決める",
  "---",
  "",
  "本文",
  "",
].join("\n");

async function main() {
  const pr = await import("../../../src/flow/paper-review.js");
  const enrich = await import("../../../src/flow/paper-review-enrich.js");
  const glab = await import("../../../src/integrations/glab-review-trends.js");
  const wcs = await import("../../../src/web-chat/session.js");
  const wct = await import("../../../src/web-chat/transcript.js");
  const blocks = await import("../../../src/flow/paper-blocks.js");
  const Database = (await import("better-sqlite3")).default;
  const { Hono } = await import("hono");

  const gamesDir = path.join(tmp, "games");
  fs.mkdirSync(gamesDir, { recursive: true });
  fs.writeFileSync(path.join(gamesDir, "monster-strike.md"), GAME_MD, "utf-8");

  const out: Record<string, unknown> = { gameMd: GAME_MD };

  // ── mergeMechanics ──
  out.merge = {
    primary: [{ name: "裁判", description: "Anatomia 由来" }, { name: " ", description: "空" }],
    secondary: [{ name: " 裁判 ", description: "落ちる" }, { name: "経済", description: "x" }, { name: "ＡＢＣ", description: "全角" }, { name: "ａｂｃ", description: "全角小" }],
  };
  (out.merge as Record<string, unknown>).out = pr.mergeMechanics(
    (out.merge as { primary: never[] }).primary,
    (out.merge as { secondary: never[] }).secondary
  );

  // ── assessPaperUnderstanding ──
  const seedFull = { gameTitle: "G", discussionTheme: "T", discussionContent: "C", mechanicsContext: "M", themeSupplement: "S" };
  const seedThin = { gameTitle: "", discussionTheme: "T", discussionContent: "", mechanicsContext: "", themeSupplement: "" };
  const mech1 = [{ name: "A", description: "a", source: "curated" }];
  const voices = Array.from({ length: 7 }, (_, i) => ({ content: `声${i} ` + "長".repeat(i === 0 ? 170 : 3), source: i % 2 ? "youtube" : "niconico" }));
  const understandingCases: Array<{ fields: unknown; mechanics: unknown[]; voices: unknown[]; flow?: string; model?: string; responses: Resp[] | null }> = [
    { fields: seedThin, mechanics: [], voices: [], flow: "improvement", responses: null },
    { fields: seedThin, mechanics: mech1, voices: [], flow: "discussion", responses: null },
    { fields: seedThin, mechanics: [], voices, flow: undefined, responses: null },
    { fields: seedFull, mechanics: mech1, voices, flow: "improvement", model: "m1", responses: [ok('前置き {"ok":false,"rationale":"  足りない ","missingQuestions":[" q1 ","",3,"q2"]} 後置き')] },
    { fields: seedFull, mechanics: mech1, voices, flow: "discussion", responses: [ok('{"missingQuestions":[]}')] },
    { fields: seedFull, mechanics: [], voices: [], flow: "improvement", responses: [ok('{"ok":"yes","missingQuestions":["x"]}')] },
    { fields: seedThin, mechanics: [], voices: [], flow: "improvement", responses: [ng("down")] },
    { fields: seedThin, mechanics: [], voices: [], flow: "improvement", responses: [ok("JSON ではない")] },
    { fields: seedThin, mechanics: [], voices: [], flow: "improvement", responses: [boom("timeout")] },
    { fields: seedFull, mechanics: [], voices: [], flow: "learning", responses: [ok('{"ok":true,"rationale":""}')] },
  ];
  out.understanding = [];
  for (const c of understandingCases) {
    const f = c.responses ? fakeLlm(c.responses) : null;
    const warns: string[] = [];
    const result = await pr.assessPaperUnderstanding(c.fields as never, c.mechanics as never, c.voices as never, f?.llm as never, {
      flow: c.flow, model: c.model, warn: (m: string) => warns.push(m),
    });
    (out.understanding as unknown[]).push({ ...c, result, calls: f?.calls ?? [], warns });
  }

  // ── buildPaperDraft ──
  const manyVoices = Array.from({ length: 52 }, (_, i) => ({ content: (i === 0 ? "あ".repeat(130) : `感想 ${i}`), source: i === 0 ? "niconico" : "youtube" }));
  type BuildCase = {
    name: string; theme: string; tags: string[]; flow?: string; seed?: unknown; voices?: unknown[]; useGames: boolean;
    llm?: Resp[]; understandingLlm?: Resp[]; gate?: { responses: Resp[]; min: number }; extra?: unknown[]; mechanicsTarget?: number; enrichModel?: string;
  };
  const buildCases: BuildCase[] = [
    { name: "plain", theme: "ローグライト", tags: ["開発"], voices: manyVoices.slice(0, 12), useGames: false },
    { name: "capped", theme: "ローグライト", tags: [], voices: manyVoices, useGames: false },
    { name: "confidential", theme: "モンスターストライク", tags: ["機密"], voices: manyVoices, useGames: true },
    { name: "discussion-seed", theme: "長寿の理由", tags: [], flow: "discussion", useGames: true,
      seed: { gameTitle: " モンスターストライク ", discussionTheme: "  ", discussionContent: " 内容 ", mechanicsContext: "", themeSupplement: " 補足 " },
      llm: [ok('{"mechanics":[{"name":"LLM増補","description":"仮説"}]}')], understandingLlm: [ok('{"ok":true,"rationale":"分かる","missingQuestions":[]}')],
      extra: [{ name: "ギミック", description: "使い回し由来", source: "curated" }] },
    { name: "discussion-no-seed", theme: "モンスターストライク", tags: [], flow: "discussion", useGames: true },
    { name: "enrich", theme: "モンスターストライク", tags: ["運用"], flow: "improvement", useGames: true, voices: manyVoices.slice(0, 4),
      llm: [ok('[{"name":"周回","description":"同じクエストを回す","intended_affect":"達成"},{"name":"ギミック","description":"重複"}]')],
      mechanicsTarget: 4, enrichModel: "small" },
    { name: "seed-understanding-fallback", theme: "改善したい", tags: [], flow: "improvement", useGames: false,
      seed: { gameTitle: "", discussionTheme: "改善テーマ", discussionContent: "", mechanicsContext: "", themeSupplement: "" },
      llm: [ok("[]"), ok('{"ok":false,"rationale":"","missingQuestions":["基本ループは?"]}')], voices: manyVoices.slice(0, 2), enrichModel: "" },
    { name: "gate", theme: "架空のテーマXYZ", tags: [], useGames: false,
      voices: [{ content: "好き 1", source: "a" }, { content: "嫌い 1", source: "b" }, { content: "好き 2", source: "a" }],
      gate: { responses: [ok(JSON.stringify(["ガチャ緩和は収益を毀損するか", "天井はライト層の課金動機を下げるか"])), ok(JSON.stringify([{ index: 1, armable: "both" }, { index: 2, armable: "pro_only" }]))], min: 2 } },
    { name: "gate-degraded", theme: "架空", tags: ["開発"], useGames: false, voices: [], gate: { responses: [ng("down")], min: 2 } },
  ];
  out.build = [];
  for (const c of buildCases) {
    const llm = c.llm ? fakeLlm(c.llm) : null;
    const ullm = c.understandingLlm ? fakeLlm(c.understandingLlm) : null;
    const gate = c.gate ? fakeLlm(c.gate.responses) : null;
    const warns: string[] = [];
    const listCalls: unknown[] = [];
    const r = await pr.buildPaperDraft(c.theme, c.tags as never, {
      flow: c.flow,
      gamesDir: c.useGames ? gamesDir : path.join(tmp, "no-such-games"),
      listExternalVoices: c.voices ? (terms: string[], limit: number) => { listCalls.push({ terms, limit }); return c.voices as never; } : undefined,
      llm: llm?.llm as never,
      understandingLlm: ullm?.llm as never,
      mechanicsTarget: c.mechanicsTarget,
      enrichModel: c.enrichModel,
      extraMechanics: c.extra as never,
      seed: c.seed as never,
      debatability: gate ? { llm: gate.llm as never, minArmableIssues: c.gate!.min } : undefined,
      warn: (m: string) => warns.push(m),
    });
    (out.build as unknown[]).push({
      ...c, draft: r.draft, info: r.info, llmCalls: llm?.calls ?? [], understandingCalls: ullm?.calls ?? [], gateCalls: gate?.calls ?? [], listCalls,
    });
  }

  // ── applyPaperEdit ──
  const baseDraft = pr.withDerivedBody({ theme: "T", tags: ["開発"], supplement: "S", mechanics: [{ name: "M", description: "d", intended_affect: "喜", source: "crawl" }, { name: "N", description: "" }], issues: ["i1"] } as never);
  const fixedDraft = pr.withDerivedBody({ theme: "固定T", tags: [], supplement: "旧", mechanics: [], gameTitle: "G", discussionTheme: "固定T", discussionContent: "C", mechanicsContext: "MC", themeSupplement: "TS" } as never);
  const editCases: Array<{ draft: unknown; instruction: string; responses: Resp[]; model?: string }> = [
    { draft: baseDraft, instruction: "  メカニクスにガチャを追加 ", model: "edit-model", responses: [ok(JSON.stringify({ theme: "T2", tags: ["開発", "運用", "存在しない", "開発"], supplement: " S2 ", mechanics: [{ name: " ガチャ ", description: " 確率 ", intended_affect: " 射幸 ", source: "llm" }, { description: "名前なし" }, { name: "X", source: "bogus" }], changeSummary: " 追加 " }))] },
    { draft: baseDraft, instruction: "フェンス", responses: [ok("はい:\n```json\n{\"theme\":\"\",\"supplement\":\"S3\",\"mechanics\":[],\"changeSummary\":\"\"}\n```")] },
    { draft: baseDraft, instruction: "曖昧", responses: [ok("ごめんなさい")] },
    { draft: baseDraft, instruction: "失敗", responses: [ng("rate limited")] },
    { draft: baseDraft, instruction: "例外", responses: [boom("socket hang up")] },
    { draft: baseDraft, instruction: "   ", responses: [ok("{}")] },
    { draft: baseDraft, instruction: "空 JSON", responses: [ok("{}")] },
    { draft: baseDraft, instruction: "issues 不正", responses: [ok('{"issues":"文字列","tags":null,"mechanics":"x"}')] },
    { draft: fixedDraft, instruction: "固定欄", responses: [ok(JSON.stringify({ gameTitle: " 新G ", discussionTheme: " 新テーマ ", discussionContent: "", themeSupplement: " 新補足 ", supplement: "無視される", issues: [" a ", 1, ""] }))] },
    { draft: fixedDraft, instruction: "固定欄2", responses: [ok(JSON.stringify({ discussionTheme: "  ", theme: " 旧来テーマ ", themeSupplement: "" }))] },
    { draft: baseDraft, instruction: "配列", responses: [ok('[{"theme":"x"}]')] },
    { draft: baseDraft, instruction: "壊れ", responses: [ok('{"theme": "x", } trailing }')] },
  ];
  out.edit = [];
  for (const c of editCases) {
    const f = fakeLlm(c.responses);
    const warns: string[] = [];
    const result = await pr.applyPaperEdit(c.draft as never, c.instruction, f.llm as never, { model: c.model, warn: (m: string) => warns.push(m) });
    (out.edit as unknown[]).push({ ...c, result, calls: f.calls, warns });
  }

  // ── coercePaperDraft ──
  const fb = pr.withDerivedBody({ theme: "FB", tags: ["開発"], supplement: "FBS", mechanics: [{ name: "FM", description: "d" }], issues: ["fi"], gameTitle: "FG" } as never);
  const raws: unknown[] = [
    null, 3, "str", [], { theme: "  新議題  ", tags: ["運用", "bad", "運用"], mechanics: [{ name: "A", description: "x" }, { description: "名前なし" }] },
    { supplement: 5, mechanics: "x", issues: [" a ", ""] }, { tags: null }, { theme: "x", issues: null },
    { bodyMd: "# 議題\n本文の議題\n\n# 観点補足\n補足\n\n# ゲームのメカニクス\n- **B**: b （出所: crawl）\n", tags: ["機密"] },
    { bodyMd: "   ", theme: "空 md は無視" }, { bodyMd: fb.bodyMd },
  ];
  out.coerce = { fallback: fb, cases: raws.map((raw) => ({ raw, result: pr.coercePaperDraft(raw, fb) })) };

  // ── renderPaperReview ──
  const rich = pr.withDerivedBody({ theme: "ローグライト", tags: ["開発", "運用"], supplement: "開発者観点で", mechanics: [
    { name: "ビルド選択", description: "構成を変える", intended_affect: "発見" }, { name: "周回", description: "回す", source: "crawl" },
    { name: "仮説", description: "LLM", source: "llm", intended_affect: "高揚" }, { name: "仮説2", description: "", source: "llm" },
  ], issues: ["論点A", "論点B"] } as never);
  const bare = pr.withDerivedBody({ theme: "空", tags: [], supplement: "", mechanics: [] } as never);
  const deb = (o: Record<string, unknown>) => ({ issues: [], armability: [], armableBothCount: 0, minArmableIssues: 2, evidence: { voiceCount: 0, positive: 0, negative: 0, neutral: 0, polaritySkew: 0, sourceShares: [], dominantSource: null }, debatable: true, degraded: false, message: "m", ...o });
  const renderCases = [
    { draft: rich, info: { voiceCount: 50, countCapped: true, samples: [{ content: "楽しい", source: "niconico" }] } },
    { draft: bare, info: { voiceCount: 0, countCapped: false, samples: [] } },
    { draft: bare, info: { voiceCount: 1, countCapped: false, samples: [], debatability: deb({ message: "議論できます" }) } },
    { draft: bare, info: { voiceCount: 1, countCapped: false, samples: [], debatability: deb({ debatable: false, message: "不適", recommendation: { flow: "sparring", reason: "r" } }) } },
    { draft: bare, info: { voiceCount: 1, countCapped: false, samples: [], debatability: deb({ debatable: false, message: "不適2" }) } },
    { draft: bare, info: { voiceCount: 1, countCapped: false, samples: [], debatability: deb({ degraded: true, message: "失敗" }) } },
  ];
  out.render = renderCases.map((c) => ({ ...c, md: pr.renderPaperReview(c.draft as never, c.info as never) }));

  // ── 承認語 / 戻す語 ──
  const words = ["開始", "承認", "OK", "ok。", "ok!!", "Ok！ ", "これで開始", "これでOK", "✅", "👍", "go", " GO. ", "approve", "", "  ", "。", "メカニクスにガチャを追加",
    "開始したい理由は", "戻す", "戻して", "undo", "REVERT", "↶", "↩", "取り消し", "取消。", "メカニクスを戻して具体化", "ＯＫ", "start　", "start !"];
  out.words = words.map((w) => ({ text: w, approval: pr.isApprovalText(w), revert: pr.isRevertText(w) }));

  // ── reviewBlock ──
  const md = bare.bodyMd;
  const bl = blocks.splitBlocks(md);
  const para = bl.find((b: { type: string }) => b.type === "paragraph")!;
  const reviewCases: Array<{ blockId: string; instruction?: string; model?: string; responses: Resp[] }> = [
    { blockId: para.id, responses: [ok(JSON.stringify({ proposed: " 改稿 ", rationale: " 具体化 " }))] },
    { blockId: para.id, instruction: " もっと具体的に ", model: "rv", responses: [ok('{"proposed":"x"}')] },
    { blockId: para.id, responses: [ok('{"proposed":"  "}')] },
    { blockId: para.id, responses: [ok("no json")] },
    { blockId: para.id, responses: [ng("err")] },
    { blockId: para.id, responses: [boom("thrown")] },
    { blockId: "b999", responses: [ok("{}")] },
  ];
  out.review = { bodyMd: md, cases: [] as unknown[] };
  for (const c of reviewCases) {
    const f = fakeLlm(c.responses);
    const warns: string[] = [];
    const result = await pr.reviewBlock({ bodyMd: md, blockId: c.blockId, instruction: c.instruction, model: c.model, llm: f.llm as never, warn: (m: string) => warns.push(m) });
    (out.review as { cases: unknown[] }).cases.push({ ...c, result, calls: f.calls, warns });
  }

  // ── gatherEvidence ──
  const evVoices = [{ content: "ガチャが楽しい", source: "niconico" }, { content: "い".repeat(205), source: "youtube" }, { content: "三件目", source: "steam" }];
  const evCases: Array<{ topic: string; voices: unknown[]; limit?: number; responses: Resp[] | null; model?: string }> = [
    { topic: "ガチャ", voices: evVoices, responses: null },
    { topic: "無", voices: [], responses: [ok("呼ばれない")] },
    { topic: "ガチャ", voices: evVoices, responses: [ok(" ガチャは賛否が割れる要素。 ")], model: "ev" },
    { topic: "ガチャ", voices: evVoices, responses: [ok("   ")] },
    { topic: "ガチャ", voices: evVoices, responses: [ng("x")] },
    { topic: "ガチャ", voices: evVoices, responses: [boom("y")], limit: 2 },
  ];
  out.evidence = [];
  for (const c of evCases) {
    const f = c.responses ? fakeLlm(c.responses) : null;
    const listCalls: unknown[] = [];
    const warns: string[] = [];
    const result = await pr.gatherEvidence({ topic: c.topic, listExternalVoices: (t: string[], l: number) => { listCalls.push({ terms: t, limit: l }); return c.voices as never; }, llm: f?.llm as never, model: c.model, limit: c.limit, warn: (m: string) => warns.push(m) });
    (out.evidence as unknown[]).push({ ...c, result, calls: f?.calls ?? [], listCalls, warns });
  }

  // ── paper-review-enrich ──
  type EnrichCase = { intent: unknown; crawl?: number | "throw"; voices?: unknown[] | null; investigate?: Record<string, unknown[] | "throw">; llm?: Resp[] };
  const enrichCases: EnrichCase[] = [
    { intent: { kind: "adjust", text: "簡単に", terms: [] } },
    { intent: { kind: "voices", text: "外部の声を集めて", terms: ["ガチャ", "天井"] }, crawl: 3, voices: evVoices },
    { intent: { kind: "voices", text: "口コミ", terms: [] }, crawl: "throw", voices: [] },
    { intent: { kind: "voices", text: "口コミ", terms: [] }, voices: null },
    { intent: { kind: "voices", text: "口コミ", terms: [] }, crawl: 0, voices: evVoices, llm: [ok("要約段落")] },
    { intent: { kind: "similar_games", text: "類似ゲーム", terms: ["A", "B", "C", "D", "E", "F"] },
      investigate: { A: [{ name: "a1", description: "" }, ...Array.from({ length: 9 }, (_, i) => ({ name: `a${i + 2}`, description: `d${i}` }))], B: [], C: "throw" } },
    { intent: { kind: "similar_games", text: "似たゲーム", terms: [] } },
    { intent: { kind: "similar_games", text: "代替", terms: ["X"] }, investigate: { X: [] } },
  ];
  out.enrich = [];
  for (const c of enrichCases) {
    const crawlCalls: string[] = [];
    const invCalls: unknown[] = [];
    const warns: string[] = [];
    const f = c.llm ? fakeLlm(c.llm) : null;
    const result = await enrich.buildIntentInstruction(c.intent as never, {
      theme: "議題X",
      tags: ["開発"],
      gamesDir: "/no/such",
      listExternalVoices: c.voices === null || c.voices === undefined ? undefined : (() => c.voices) as never,
      llm: f?.llm as never,
      crawl: c.crawl === undefined ? undefined : async (q: string) => { crawlCalls.push(q); if (c.crawl === "throw") throw new Error("crawl down"); return { imported: c.crawl as number }; },
      investigate: (async (a: { theme: string; tags: string[]; gamesDir?: string }) => {
        invCalls.push({ theme: a.theme, tags: a.tags, gamesDir: a.gamesDir ?? null });
        const v = c.investigate?.[a.theme];
        if (v === "throw") throw new Error("inv down");
        return { mechanics: v ?? [], sentimentCount: 0, youtubeUsed: false, youtubeComments: [] };
      }) as never,
      warn: (m: string) => warns.push(m),
    });
    (out.enrich as unknown[]).push({ ...c, result, crawlCalls, invCalls, warns, calls: f?.calls ?? [] });
  }

  // ── glab review trends ──
  const now = Date.parse("2026-08-25T00:00:00.000Z");
  const day = 24 * 60 * 60 * 1000;
  const records = [
    { gameSlug: "alpha", appId: 10, postedAt: now - 60_000, reaction: "positive" },
    { gameSlug: "alpha", appId: 10, postedAt: now - 120_000, reaction: "negative" },
    { gameSlug: "alpha", appId: 10, postedAt: now - 130_000, reaction: "positive" },
    { gameSlug: " beta ", appId: 20, postedAt: now - 180_000, reaction: "positive" },
    { gameSlug: "beta", appId: 21, postedAt: now - 190_000, reaction: null },
    { gameSlug: "gamma", appId: null, postedAt: now - 7 * day, reaction: null },
    { gameSlug: "delta", appId: 40, postedAt: now - 180_000, reaction: "negative" },
    { gameSlug: "epsilon", appId: 50, postedAt: now - 180_000, reaction: "positive" },
    { gameSlug: "old", appId: 30, postedAt: now - 8 * day, reaction: "positive" },
    { gameSlug: "future", appId: 30, postedAt: now + 1, reaction: "positive" },
    { gameSlug: null, appId: 1, postedAt: now - 1, reaction: "positive" },
    { gameSlug: "  ", appId: 1, postedAt: now - 1, reaction: "positive" },
  ];
  out.trends = [
    { records, now, limit: 8, result: glab.analyzeStoredReviewTrends(records as never, now, 8) },
    { records, now, limit: 2, result: glab.analyzeStoredReviewTrends(records as never, now, 2) },
    { records, now, limit: -1, result: glab.analyzeStoredReviewTrends(records as never, now, -1) },
  ];
  const urls = [
    "https://steamcommunity.com/profiles/1/recommended/1245620/", "https://steamcommunity.com/id/x/recommended/1245620",
    "https://store.steampowered.com/app/10/Name/", "https://STORE.steampowered.com/app/0/", "http://store.steampowered.com/app/10/",
    "https://example.com/recommended/1245620/", "https://steamcommunity.com/recommended/12x/", "https://steamcommunity.com/app/12x/app/34",
    "https://steamcommunity.com/app/9007199254740991", "https://steamcommunity.com/app/9007199254740992", "https://steamcommunity.com/app/007/",
    "not a url", "", "https://steamcommunity.com/apps/10/", "https://steamcommunity.com/app//10", "https://steamcommunity.com:443/app/77?x=1",
  ];
  out.appIds = urls.map((u) => ({ url: u, appId: glab.steamAppIdFromSourceUrl(u) }));

  // ── web-chat ──
  out.rooms = ["", "  lobby2 ", "a b/c?d=e", "日本語room", "x".repeat(40), "...", "A-Z_a.z-09", "🙂"].map((r) => ({ raw: r, id: wcs.sanitizeRoomId(r) }));
  (out.rooms as unknown[]).push({ raw: null, id: wcs.sanitizeRoomId(null) });
  const db = new Database(":memory:");
  db.exec(`CREATE TABLE sessions (id TEXT PRIMARY KEY, workspace_id TEXT, title TEXT, scene TEXT, started_at INTEGER);
           CREATE TABLE utterances (id TEXT PRIMARY KEY, session_id TEXT, speaker_id TEXT, raw_content TEXT, posted_at INTEGER);`);
  const sessions = [
    ["s-in", "knowledge", "web-session:r1", "web:r1", 1],
    ["s-gap", "knowledge", "discussion-of-gap:g1", "web:r1", 2],
    ["s-other", "knowledge", "web-session:r2", "web:r2", 3],
    ["s-ws", "other", "web-session:r1", "web:r1", 4],
  ];
  const utterances = [
    ["u1", "s-in", "太郎", "こんにちは", 100],
    ["u2", "s-gap", "太郎", "ミラー", 100],
    ["u3", "s-gap", "persona:p1", "ペルソナ発話", 110],
    ["u4", "s-gap", null, "進行", 120],
    ["u5", "s-gap", "ext:steam:1", "外部", 130],
    ["u6", "s-in", "persona:p2", "入口の非人間", 140],
    ["u7", "s-other", "花子", "別 room", 150],
    ["u8", "s-ws", "次郎", "別 workspace", 160],
    ["u0", "s-in", "", "空 speaker", 160],
    ["u9", "s-gap", "persona:", "空 persona", 170],
  ];
  for (const s of sessions) db.prepare("INSERT INTO sessions VALUES (?, ?, ?, ?, ?)").run(...s);
  for (const u of utterances) db.prepare("INSERT INTO utterances VALUES (?, ?, ?, ?, ?)").run(...u);
  const core = { client: { raw: db } } as never;
  out.transcript = {
    sessions, utterances,
    cases: [
      { options: {}, result: wct.readRoomTranscript(core, "knowledge", "r1") },
      { options: { sinceTs: 120, limit: 3 }, result: wct.readRoomTranscript(core, "knowledge", "r1", { sinceTs: 120, limit: 3 }) },
      { options: { resolve: true }, result: wct.readRoomTranscript(core, "knowledge", "r1", { resolveName: (id: string) => `名:${id}` }) },
      { options: { room: "r2" }, result: wct.readRoomTranscript(core, "knowledge", "r2") },
    ],
  };
  db.close();

  // ── 画面 (HTML) ──
  const { personaRoutes } = await import("../../../src/api/persona-routes.js");
  const { createNoiseRoutes } = await import("../../../src/api/noise-routes.js");
  const app = new Hono();
  app.use("*", async (c, next) => { c.set("userId" as never, "admin" as never); c.set("userRole" as never, "admin" as never); await next(); });
  app.route("/api", personaRoutes);
  app.route("/api", createNoiseRoutes());
  const webDir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "..", "web", "api");
  fs.writeFileSync(path.join(webDir, "personas.html"), await (await app.request("/api/admin/personas")).text());
  fs.writeFileSync(path.join(webDir, "noise.html"), await (await app.request("/api/admin/noise")).text());

  const dir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "w3o");
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, "golden.json"), JSON.stringify(out, null, 2) + "\n");
  fs.rmSync(tmp, { recursive: true, force: true });
  console.log("w3-O golden written");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
