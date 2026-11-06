// w1-F 担当分 (画面・管理 API の基礎・可視化の基礎・認証ミドルウェア) の Node 実装の入出力を記録する。
// 実行: npx tsx native/tests/golden/w1-F-golden.gen.ts
// 出力:
//   native/web/{flow/page.html, api/*.html}  … 画面 (Node が返す文字列をそのまま。C++ は移植しない)
//   native/tests/fixtures/w1f/*.json          … C++ テストが同じ出力を返すかを確かめる記録
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import crypto from "node:crypto";
import { Hono } from "hono";
import Database from "better-sqlite3";

delete process.env.DISCUTERE_YOUTUBE_API_KEY;

const here = path.dirname(new URL(import.meta.url).pathname);
const nativeDir = path.join(here, "..", "..");
const fixturesDir = path.join(nativeDir, "tests", "fixtures", "w1f");
fs.mkdirSync(fixturesDir, { recursive: true });
const writeFixture = (name: string, value: unknown) =>
  fs.writeFileSync(path.join(fixturesDir, name), JSON.stringify(value, null, 2) + "\n");

const { FLOW_HTML } = await import("../../../src/flow/web/page.js");
const { PUBLICATION_EDITOR } = await import("../../../src/api/conclusion-publication-editor.js");
const { dashboardRoutes } = await import("../../../src/api/dashboard-routes.js");
const { errorsRoutes } = await import("../../../src/api/errors-routes.js");
const { topPageRoutes } = await import("../../../src/api/top-page-routes.js");
const { tuningRoutes, setRuntimeSettings, setRuleReseeder } = await import("../../../src/api/tuning-routes.js");
const auth = await import("../../../src/middleware/auth.js");
const { installConsoleCapture } = await import("../../../src/observability/console-capture.js");
const errorBuffer = await import("../../../src/observability/error-buffer.js");
const store = await import("../../../src/runtime-settings/store.js");
const consensus = await import("../../../src/api/consensus-persona-aggregation.js");
const prompts = await import("../../../src/flow/foresight/prompts.js");
const markdown = await import("../../../src/flow/foresight/markdown.js");
const steam = await import("../../../src/flow/user-voices/steam-app.js");
const wikilink = await import("../../../src/visualize/wikilink.js");
const dataSources = await import("../../../src/visualize/data-sources.js");
const topicOpinions = await import("../../../src/visualize/topic-opinions.js");
const learningLayers = await import("../../../src/visualize/learning-layers.js");
const { VOICE_REFERENCE_POLICY } = await import("../../../src/flow/user-voices/reference-policy.js");

const ADMIN = { "X-User-Id": "golden", "X-User-Role": "admin" };

// ─── 画面 ────────────────────────────────────────────────────────────────
const assets: Array<{ name: string; html: string }> = [
  { name: "flow/page.html", html: FLOW_HTML },
  { name: "api/conclusion-publication-editor.html", html: PUBLICATION_EDITOR },
  { name: "api/dashboard.html", html: await (await dashboardRoutes.request("/admin/dashboard", { headers: ADMIN })).text() },
  { name: "api/top-page.html", html: await (await topPageRoutes.request("/")).text() },
  { name: "api/tuning.html", html: await (await tuningRoutes.request("/admin/tuning")).text() },
];
const assetRecords = assets.map(({ name, html }) => {
  const file = path.join(nativeDir, "web", name);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, html);
  return { name, bytes: Buffer.byteLength(html), sha256: crypto.createHash("sha256").update(html).digest("hex") };
});
writeFixture("web_assets.json", assetRecords);

// ─── 認証ミドルウェア (userContext / requireRole / isLoopbackRequest) ──────────
{
  const app = new Hono();
  app.use("*", auth.userContext());
  app.get("/who", (c) => c.json({ userId: auth.getUserId(c), userRole: auth.getUserRole(c), loopback: auth.isLoopbackRequest(c) }));
  app.use("/admin-only", auth.requireRole("admin"));
  app.get("/admin-only", (c) => c.json({ ok: true }));
  app.route("/api", dashboardRoutes);
  app.route("/api", errorsRoutes);
  const addresses = ["127.0.0.1", "::1", "::ffff:127.0.0.1", "192.168.1.10", "10.0.0.1", "::ffff:192.168.1.10",
    "127.0.0.2", "0:0:0:0:0:0:0:1", "", "localhost", "8.8.8.8"];
  const headerSets: Array<Record<string, string>> = [
    {},
    { "X-User-Id": "u1" },
    { "X-User-Id": "u1", "X-User-Role": "admin" },
    { "X-User-Role": "admin" },
    { "X-User-Id": "", "X-User-Role": "admin" },
    { "X-User-Id": "u2", "X-User-Role": "" },
    { "X-User-Id": "u3", "X-User-Role": "Admin" },
    { "x-user-id": "lower", "x-user-role": "admin" },
  ];
  const cases = [];
  for (const remoteAddress of addresses) {
    for (const headers of headerSets) {
      const env = { incoming: { socket: { remoteAddress: remoteAddress || undefined } } };
      const result: Record<string, unknown> = { remoteAddress, headers };
      for (const p of ["/who", "/admin-only", "/api/admin/dashboard", "/api/admin/errors?limit=1"]) {
        const res = await app.request(p, { headers }, env);
        const text = await res.text();
        result[p] = { status: res.status, body: res.headers.get("content-type")?.includes("json") ? JSON.parse(text) : null };
      }
      cases.push(result);
    }
  }
  // accessor の挙動 (userContext を通さない場合: ヘッダへの退避)
  const bare = new Hono();
  bare.get("/acc", (c) => c.json({ userId: auth.getUserId(c), userRole: auth.getUserRole(c), userName: auth.getUserName(c), userEmail: auth.getUserEmail(c) }));
  const accessors = [];
  for (const headers of headerSets) {
    const res = await bare.request("/acc", { headers });
    accessors.push({ headers, body: await res.json() });
  }
  // dashboard / errors を userContext 無しで (ヘッダだけで判定)
  const routesOnly = [];
  for (const headers of headerSets) {
    const d = await dashboardRoutes.request("/admin/dashboard", { headers });
    const e = await errorsRoutes.request("/admin/errors/clear", { method: "POST", headers });
    routesOnly.push({
      headers,
      dashboard: { status: d.status, json: d.status === 200 ? null : await d.json() },
      clear: { status: e.status, json: await e.json() },
    });
  }
  writeFixture("auth.json", { cases, accessors, routesOnly });
}

// ─── console-capture + error-buffer + errors-routes ──────────────────────────
{
  installConsoleCapture();
  errorBuffer.clearErrors();
  const origErr = process.stderr.write.bind(process.stderr);
  // 標準エラーへの複写は捨てる (記録だけ取る)
  (process.stderr as unknown as { write: unknown }).write = () => true;
  const messages: Array<["error" | "warn", string]> = [
    ["error", "[flow] 失敗しました"],
    ["warn", "auth: 拒否"],
    ["error", "plain message"],
    ["warn", "  [persona-engine] tick 遅延  "],
    ["error", "[] 空の括弧"],
    ["error", "[a]b] 入れ子"],
    ["warn", "Mixed-Case_9: x"],
    ["error", "日本語: コロン"],
    ["error", "no colon here: but later"],
    ["warn", "-dash: y"],
    ["error", ":leading"],
    ["error", ""],
    ["warn", "[[nested]]"],
  ];
  for (const [level, m] of messages) (level === "error" ? console.error : console.warn)(m);
  (process.stderr as unknown as { write: unknown }).write = origErr;
  const recorded = errorBuffer.recentErrors().map(({ level, source, message }) => ({ level, source, message }));
  const limits = [];
  for (const q of ["", "?limit=1", "?limit=0", "?limit=", "?limit=abc", "?limit=1.5", "?limit=300", "?limit=-5", "?limit=2.7", "?limit=Infinity", "?limit=%201%20"]) {
    const res = await errorsRoutes.request("/admin/errors" + q, { headers: ADMIN });
    const body = (await res.json()) as { errors: Array<{ level: string; source: string; message: string }> };
    limits.push({ query: q, status: res.status, messages: body.errors.map((e) => e.message) });
  }
  const clear = await errorsRoutes.request("/admin/errors/clear", { method: "POST", headers: ADMIN });
  writeFixture("errors.json", { messages, recorded, limits, clear: { status: clear.status, json: await clear.json() } });
}

// ─── runtime-settings ───────────────────────────────────────────────────────
{
  const DEF = { tickMs: 30000, idleGapMs: 120000, maxPersonas: 20, aufhebungTarget: 3, convergePolicy: "default" as const };
  const CR = { enabled: false, anatomiaUrl: "", concordiaUrl: "", service: "discutere" };
  const facOverrides: unknown[] = [
    null, {}, { maxPersonas: 8 }, { maxPersonas: 0 }, { maxPersonas: -3 }, { tickMs: 999 }, { tickMs: 1500.5 },
    { tickMs: 2499.5 }, { idleGapMs: "4000" }, { idleGapMs: " 5000 " }, { idleGapMs: "" }, { idleGapMs: "x" },
    { aufhebungTarget: true }, { aufhebungTarget: false }, { aufhebungTarget: null }, { aufhebungTarget: [] },
    { aufhebungTarget: [7] }, { aufhebungTarget: ["9"] }, { aufhebungTarget: {} }, { maxPersonas: 1e300 },
    { convergePolicy: "aufhebung-only" }, { convergePolicy: "bogus" }, { convergePolicy: 1 }, { maxPersonas: 2.5 },
    { maxPersonas: 3.5 }, { maxPersonas: -0.5 }, { idleGapMs: "1e4" }, { idleGapMs: "0x10" }, 5, "str", [1, 2],
  ];
  const fac = facOverrides.map((o) => ({ override: o, result: store.mergeFacilitatorTuning(DEF, o as never) }));
  const crOverrides: unknown[] = [
    null, {}, { enabled: true }, { enabled: "yes" }, { enabled: 1 }, { anatomiaUrl: " http://a " }, { service: "" },
    { service: "  " }, { service: " x " }, { concordiaUrl: 5 }, { anatomiaUrl: null }, 7,
  ];
  const cr = crOverrides.map((o) => ({ override: o, result: store.mergeCostRelay(CR, o as never) }));

  // tuning-routes: 要求の並び → 応答
  const steps: Array<{ method: string; path: string; body?: string }> = [
    { method: "GET", path: "/admin/tuning/data" },
    { method: "PUT", path: "/admin/tuning/facilitator", body: "{}" },
  ];
  const before = [];
  for (const s of steps) before.push(await runStep(s));
  const db = new Database(":memory:");
  const st = store.createRuntimeSettingsStore(db, {
    facilitator: DEF,
    rolePrompts: { ファシリテーター: "司会の既定", 意見屋: "意見屋の既定", "5": "数字キー" },
    ruleInstructions: { "tick-con-opus": "否定の既定", "tick-pro": "肯定の既定" },
    costRelay: CR,
  });
  setRuntimeSettings(st);
  let reseeds = 0;
  setRuleReseeder(() => { reseeds += 1; });
  const seq: Array<{ method: string; path: string; body?: string }> = [
    { method: "GET", path: "/admin/tuning/data" },
    { method: "PUT", path: "/admin/tuning/facilitator", body: JSON.stringify({ maxPersonas: 12, convergePolicy: "aufhebung-only" }) },
    { method: "PUT", path: "/admin/tuning/facilitator", body: JSON.stringify({ tickMs: "abc", idleGapMs: "2000", extra: 1 }) },
    { method: "PUT", path: "/admin/tuning/facilitator", body: JSON.stringify({ aufhebungTarget: null, convergePolicy: "bad" }) },
    { method: "PUT", path: "/admin/tuning/facilitator", body: "not json" },
    { method: "PUT", path: "/admin/tuning/facilitator", body: "null" },
    { method: "PUT", path: "/admin/tuning/facilitator", body: "[1]" },
    { method: "PUT", path: "/admin/tuning/cost-relay", body: JSON.stringify({ enabled: 1, concordiaUrl: " http://localhost:17330 " }) },
    { method: "PUT", path: "/admin/tuning/cost-relay", body: JSON.stringify({ anatomiaUrl: null, service: 42 }) },
    { method: "PUT", path: "/admin/tuning/cost-relay", body: JSON.stringify({ enabled: "", service: { a: 1 } }) },
    { method: "PUT", path: "/admin/tuning/cost-relay", body: JSON.stringify({ service: [1, "a"] }) },
    { method: "PUT", path: "/admin/tuning/role", body: JSON.stringify({ text: "x" }) },
    { method: "PUT", path: "/admin/tuning/role", body: JSON.stringify({ role: "", text: "x" }) },
    { method: "PUT", path: "/admin/tuning/role", body: JSON.stringify({ role: "ファシリテーター", text: "新しい司会指示" }) },
    { method: "PUT", path: "/admin/tuning/role", body: JSON.stringify({ role: "意見屋", text: "  " }) },
    { method: "PUT", path: "/admin/tuning/role", body: JSON.stringify({ role: "未知の役", text: "追加" }) },
    { method: "PUT", path: "/admin/tuning/role", body: JSON.stringify({ role: 5, text: "数字" }) },
    { method: "PUT", path: "/admin/tuning/role", body: JSON.stringify({ role: "意見屋", text: 3 }) },
    { method: "PUT", path: "/admin/tuning/role", body: JSON.stringify({ role: "意見屋" }) },
    { method: "PUT", path: "/admin/tuning/rule", body: JSON.stringify({ ruleId: "tick-con-opus", text: "もっと鋭く" }) },
    { method: "PUT", path: "/admin/tuning/rule", body: JSON.stringify({ ruleId: "tick-pro", text: null }) },
    { method: "PUT", path: "/admin/tuning/rule", body: JSON.stringify({ text: "x" }) },
    { method: "PUT", path: "/admin/tuning/secrets/youtube-api-key", body: "{}" },
    { method: "POST", path: "/admin/tuning/secrets/youtube-api-key/refresh", body: "{}" },
    { method: "POST", path: "/admin/tuning/facilitator", body: "{}" },
    { method: "GET", path: "/admin/tuning/data" },
  ];
  const after = [];
  for (const s of seq) after.push(await runStep(s));
  const rows = db.prepare("SELECT key, value FROM runtime_settings ORDER BY key").all();
  setRuntimeSettings(null);
  setRuleReseeder(null);
  writeFixture("runtime_settings.json", { defaults: { facilitator: DEF, costRelay: CR }, fac, cr, before, after, reseeds, rows });

  async function runStep(s: { method: string; path: string; body?: string }) {
    const res = await tuningRoutes.request(s.path, {
      method: s.method,
      headers: s.body !== undefined ? { "content-type": "application/json" } : {},
      body: s.body,
    });
    const text = await res.text();
    let json: unknown = null;
    try { json = JSON.parse(text); } catch { json = null; }
    return { ...s, status: res.status, json, text: json === null ? text : null };
  }
}

// ─── consensus-persona-aggregation + localeCompare ───────────────────────────
{
  const names = ["b", "B", "a", "A", "_x", "-x", "1", "10", "2", "あ", "ア", "か", "が", "カ", "ガ", "ぁ", "漢", "字",
    "論者#a", "論者#B", "ー", "Z", "é", "e", "f", "!", "~", "(", "x y", "xy", "ａ", "Ａ", "パ", "は", "ば", "ぱ",
    "ハ", "「", "＃", "#", "Persona 2", "persona 10", "persona-1", "ペルソナ", "ぺるそな", "保守派", "改革派", "A1", "a1",
    "", " ", "アイ", "あい", "あ い", "ab", "aB", "Ab", "AB", "ﾊﾟ"];
  const sorted = [...names].sort((x, y) => x.localeCompare(y));
  const rows = [
    { id: "u2", personaId: "persona-a", displayName: "改革派A", traits: ["慎重"], content: "second", postedAt: 20, agree: false, score: 3, reasoning: "no", opinionScore: 4 },
    { id: "u1", personaId: "persona-a", displayName: "改革派A", traits: ["慎重"], content: "first", postedAt: 10, agree: true, score: 7, reasoning: "yes", opinionScore: 2 },
    { id: "u3", personaId: "persona-b", displayName: "あいだ", traits: ["新規性重視"], content: "pending", postedAt: 15, agree: null, score: null, reasoning: null, opinionScore: 1 },
    { id: "u4", personaId: "persona-c", displayName: "Bob", traits: [], content: "c1", postedAt: 5, agree: true, score: 2.5, reasoning: null, opinionScore: 0.5 },
    { id: "u5", personaId: "persona-c", displayName: "Bob", traits: [], content: "c0", postedAt: 5, agree: true, score: null, reasoning: "tie", opinionScore: 1 },
    { id: "u6", personaId: "persona-d", displayName: "alice", traits: ["x", "y"], content: "d", postedAt: 1, agree: false, score: -1, reasoning: "", opinionScore: -2 },
    { id: "u7", personaId: "persona-e", displayName: "アイダ", traits: [], content: "e", postedAt: 2, agree: true, score: 1, reasoning: null, opinionScore: 0 },
  ];
  writeFixture("consensus.json", {
    sorted,
    rows,
    aggregated: consensus.aggregatePersonaConsensus(rows),
    empty: consensus.aggregatePersonaConsensus([]),
    ratios: [[], [{ agree: null }], [{ agree: true }, { agree: false }, { agree: null }], [{ agree: true }, { agree: true }, { agree: false }]].map((u) => consensus.ratioOfAgreed(u)),
  });
}

// ─── foresight prompts / markdown ────────────────────────────────────────────
{
  const ctx = { paperMd: "# 議題\nガチャ\n\n# 議論の経過\n止揚: 段階導入", voices: [{ content: "高すぎる", source: "steam" }] };
  const manyVoices = Array.from({ length: 10 }, (_, i) => ({ content: `声${i}`, source: i % 2 ? "youtube" : "steam" }));
  // サロゲート対の途中で切る入力 (5999 + 😀) は JS では孤立サロゲートになり UTF-8 で表せないので、境界ちょうどで切れる形にする。
  const longPaper = "あ".repeat(5998) + "😀" + "い".repeat(10);
  const lines = [
    { personaName: "A", camp: "reform" as const, text: "やるべき" },
    { personaName: "B", camp: "conservative" as const, text: "やめるべき" },
  ];
  const out = {
    contextBlocks: [
      prompts.contextBlock(undefined),
      prompts.contextBlock({ paperMd: "", voices: [] }),
      prompts.contextBlock({ paperMd: "   ", voices: manyVoices }),
      prompts.contextBlock({ paperMd: "x".repeat(6000), voices: [] }),
      prompts.contextBlock({ paperMd: "x".repeat(6001), voices: [] }),
      prompts.contextBlock({ paperMd: longPaper, voices: [] }),
      prompts.contextBlock(ctx),
    ],
    proposals: [
      prompts.buildProposalPrompt("ガチャ課金の未来", "ガチャは続く", ctx),
      prompts.buildProposalPrompt("t", ""),
      prompts.buildProposalPrompt("t", "  結論  ", { paperMd: "", voices: [] }),
    ],
    camps: [
      prompts.buildCampTurnPrompt({ theme: "t", proposal: "P", personaName: "A", camp: "reform", lines: [], ownStatements: ["前に言ったこと", "  ", " 二つ目 "] }),
      prompts.buildCampTurnPrompt({ theme: "t", proposal: "P", personaName: "B", camp: "conservative", lines, stage: "verification" }),
      prompts.buildCampTurnPrompt({ theme: "t", proposal: "P", personaName: "B", camp: "conservative", lines, stage: "refutation", ownStatements: [] }),
    ],
    forecasts: [
      prompts.buildForecastPrompt({ theme: "t", conclusion: "c", proposal: "P", lines: [], context: ctx }),
      prompts.buildForecastPrompt({ theme: "t", conclusion: "", proposal: "P", lines }),
    ],
    stances: [prompts.campStanceLine("reform"), prompts.campStanceLine("conservative")],
    stages: [prompts.campStageLine("conditions"), prompts.campStageLine("refutation"), prompts.campStageLine("verification")],
  };
  const strips = [
    "", "# 結論\nY\n\n# 未来予測アドバイザリー (参考)", "# 結論\nY\n\n# 未来予測\n旧版", "# 未来予測の補足ではない見出し\n本文",
    "# 未来予測 アドバイザリー (参考)\nx", "# 未来予測  \t\nx", "a\n# 未来予測\r\nb", "a\n  # 未来予測\nb", "本文\n\n\n  \t",
    "a\n# 未来予測アドバイザリー (参考) \nb\n# 未来予測\nc", "# 未来予測アドバイザリー(参考)\nx", "x # 未来予測\ny",
  ].map((input) => ({ input, output: markdown.stripForesight(input) }));
  const appends = [
    ["# 議題\nX\n\n# 結論\nY", { proposal: "P1", forecast: "F1" }],
    ["", { proposal: "P", forecast: "" }],
    ["# 議題\nX\n\n# 結論\nY\n\n# 未来予測アドバイザリー (参考)\n古い", { proposal: "  ", forecast: " F " }],
    ["  \n", { proposal: "P", forecast: "F" }],
  ].map(([body, section]) => ({ body, section, output: markdown.appendForesightMarkdown(body as string, section as never) }));
  writeFixture("foresight.json", { ...out, strips, appends, VOICE_REFERENCE_POLICY });
}

// ─── steam-app (normalizeGameName / isSameGame) ──────────────────────────────
{
  const names = ["モンスターストライク", "Monster Strike", "monster-strike", "ＭＯＮＳＴＥＲ　ＳＴＲＩＫＥ", "ﾓﾝｽﾀｰｽﾄﾗｲｸ",
    "ﾊﾟｽﾞﾙ&ﾄﾞﾗｺﾞﾝｽﾞ", "ファイナルファンタジーⅦ", "①②③", "Pokémon", "Pokémon", "が", "が", "ドラゴンクエスト・ウォーク",
    "「スプラトゥーン」", "『ゼルダ』【新作】", "Hello, World!", "TEST？！", "ＡＢＣ（ｄｅｆ）", "㈱ゲーム", "™Game™", "ﬁnal",
    "Ⅻ", "ÀÉÎÕÜ", "A_B-C–D—E", "  空白  あり  ", "x:y：z", "'quote\"", "[bracket]", "İstanbul", "ΣΑΣ", "Straße"];
  const normalized = names.map((n) => ({ input: n, output: steam.normalizeGameName(n) }));
  const pairs: Array<[string, string]> = [
    ["モンスターストライク", "モンスターストライクのガチャ天井を下げるか"],
    ["Monster Strike", "monster-strike"],
    ["パズル&ドラゴンズ", "モンスターストライクの天井"],
    ["A", "A の話"],
    ["AB", "ab"],
    ["ﾓﾝｽﾄ", "モンスト"],
    ["", ""],
    ["😀😀", "😀😀の話"],
    ["・・", "ab"],
    ["ab", "・・"],
  ];
  writeFixture("steam.json", { normalized, same: pairs.map(([c, q]) => ({ candidate: c, query: q, same: steam.isSameGame(c, q) })) });
}

// ─── wikilink ───────────────────────────────────────────────────────────────
{
  const parses = ["[[hyp:abc123]]", "  [[utt:550e8400]]  ", "[[unknown:foo]]", "text [[hyp:x]] tail", "[[hyp:]]", "[[HYP:x]]",
    "[[hyp:a:b]]", "[[hyp:a b]]", "[[hyp:a]b]]", "[[game:モンスト]]", "[[persona:p-1]]", "[[hyp:x]]]", "[[[hyp:x]]", "\t[[ses:s]]\n"]
    .map((input) => ({ input, output: wikilink.parseWikilink(input) }));
  const extracts = [
    "# h\n- [[hyp:a1]]\n- [[utt:b2]]\nrandom [[bad: x]] [[hyp:c3]] tail",
    "[[[hyp:a]]] [[hyp:b]][[mch:c]] [[hyp:d e]] [[Hyp:f]] [[hyp:g]",
    "[[x[[hyp:h]]", "[[aaa[[hyp:i]]", "[[hyp:j]]x]]", "[[gap:日本語]] [[aes:😀]]",
  ].map((input) => ({ input, output: wikilink.extractWikilinks(input) }));
  const formats = [
    { type: "mch", id: "ghi789" }, { type: "xxx", id: "a" }, { type: "hyp", id: "has space" }, { type: "hyp", id: "" },
    { type: "hyp", id: "a]" }, { type: "game", id: "モンスト" },
  ].map((link) => {
    try { return { link, output: wikilink.formatWikilink(link as never) }; } catch (e) { return { link, error: (e as Error).message }; }
  });
  const normalizes = ["hypothesis", "HYP", "design-gap", "designgap", "gap", "session", "ses", "nope", "Persona", "UTTERANCE", "constructor", "toString", ""]
    .map((input) => ({ input, output: wikilink.normalizeType(input) }));
  writeFixture("wikilink.json", { parses, extracts, formats, normalizes, TYPE_TO_TABLE: wikilink.TYPE_TO_TABLE, TYPE_TO_DIR: wikilink.TYPE_TO_DIR, FULL_TO_PREFIX: wikilink.FULL_TO_PREFIX, NODE_TYPES: wikilink.NODE_TYPES });
}

// ─── visualize snapshots (data-sources / topic-opinions / learning-layers) ───
{
  const schema = [
    "CREATE TABLE games (id TEXT PRIMARY KEY, workspace_id TEXT, title TEXT, genre TEXT, updated_at INTEGER)",
    "CREATE TABLE mechanics (id TEXT PRIMARY KEY, workspace_id TEXT, game_id TEXT, name TEXT, description TEXT, intends TEXT, intended_affect TEXT, updated_at INTEGER)",
    "CREATE TABLE aesthetics (id TEXT PRIMARY KEY, workspace_id TEXT, game_id TEXT, name TEXT, description TEXT, updated_at INTEGER)",
    "CREATE TABLE affects (id TEXT PRIMARY KEY, workspace_id TEXT, mood TEXT)",
    "CREATE TABLE design_gaps (id TEXT PRIMARY KEY, workspace_id TEXT, game_id TEXT, title TEXT, description TEXT, status TEXT, gap_in TEXT, expected_affect TEXT, observed_affect TEXT, updated_at INTEGER)",
    "CREATE TABLE hypotheses (id TEXT PRIMARY KEY, workspace_id TEXT, design_gap_id TEXT, statement TEXT, status TEXT, integrated INTEGER, updated_at INTEGER)",
    "CREATE TABLE sessions (id TEXT PRIMARY KEY, workspace_id TEXT, title TEXT, scene TEXT)",
    "CREATE TABLE utterances (id TEXT PRIMARY KEY, workspace_id TEXT, session_id TEXT, raw_content TEXT)",
  ];
  const inserts = [
    "INSERT INTO games VALUES ('game-1','knowledge','Sample Game','action',100)",
    "INSERT INTO games VALUES ('game-2','knowledge','Other','puzzle',300)",
    "INSERT INTO games VALUES ('game-3','knowledge','Empty',NULL,200)",
    "INSERT INTO games VALUES ('game-x','other','Elsewhere','x',999)",
    "INSERT INTO mechanics VALUES ('m1','knowledge','game-1','Dash','fast movement','make traversal confident','accomplishment',120)",
    "INSERT INTO mechanics VALUES ('m2','knowledge','game-1','Reward Loop',NULL,NULL,'accomplishment',110)",
    "INSERT INTO mechanics VALUES ('m3','knowledge','game-2','Match','three','calm','calm',310)",
    "INSERT INTO aesthetics VALUES ('a1','knowledge','game-1','Speed',NULL,105)",
    "INSERT INTO aesthetics VALUES ('a2','knowledge','game-2','Calm','quiet',305)",
    "INSERT INTO affects VALUES ('af1','knowledge','accomplishment')",
    "INSERT INTO affects VALUES ('af2','knowledge','calm')",
    "INSERT INTO design_gaps VALUES ('g1','knowledge','game-1','Dash feels unclear',NULL,'open','m1','accomplishment','confusion',130)",
    "INSERT INTO design_gaps VALUES ('g-big','knowledge','game-2','オンボーディングが長い','初回体験の離脱が多い','open','m1','confidence','confusion',200)",
    "INSERT INTO design_gaps VALUES ('g-small','knowledge',NULL,'報酬が弱い',NULL,'open',NULL,NULL,NULL,300)",
    "INSERT INTO design_gaps VALUES ('g-tie','knowledge',NULL,'同点',NULL,NULL,NULL,NULL,NULL,300)",
    "INSERT INTO hypotheses VALUES ('h1','knowledge','g1','Add a stronger dash trail','proposed',0,140)",
    "INSERT INTO hypotheses VALUES ('h2','knowledge','g-big','最初の選択肢を二つに絞る','proposed',0,220)",
    "INSERT INTO hypotheses VALUES ('h3','knowledge','g-big','説明をプレイ中の短い提示へ移す','integrated',1,230)",
    "INSERT INTO hypotheses VALUES ('h4','knowledge','g-small','短期報酬を追加する',NULL,0,240)",
    "INSERT INTO hypotheses VALUES ('h5','knowledge','g-small','real で 1',NULL,1.0,250)",
    "INSERT INTO sessions VALUES ('s1','knowledge','website:shadow-of-the-colossus',NULL)",
    "INSERT INTO sessions VALUES ('s2','knowledge','website:shadow-of-the-colossus',NULL)",
    "INSERT INTO sessions VALUES ('s3','knowledge','youtube:shadow-of-the-colossus',NULL)",
    "INSERT INTO sessions VALUES ('s4','knowledge','steam:vampire-survivors',NULL)",
    "INSERT INTO sessions VALUES ('s5','knowledge','discussion-of-gap:3460FDC0-9a31-4f90-bcd6-979fb9073157',NULL)",
    "INSERT INTO sessions VALUES ('s6','knowledge',NULL,'discord:guild/thread')",
    "INSERT INTO sessions VALUES ('s7','knowledge',NULL,'gap:g-big')",
    "INSERT INTO sessions VALUES ('s8','knowledge',NULL,NULL)",
    "INSERT INTO sessions VALUES ('s10','knowledge','plaintitle',NULL)",
    "INSERT INTO sessions VALUES ('s11','knowledge','newsource:(gap)',NULL)",
    "INSERT INTO sessions VALUES ('s12','knowledge','discussion-of-gap:g-big','gap:g-big')",
    "INSERT INTO sessions VALUES ('s13','knowledge','reddit:b-game',NULL)",
    "INSERT INTO sessions VALUES ('s14','knowledge','fandom:a-game',NULL)",
    "INSERT INTO sessions VALUES ('s9','other','steam:x',NULL)",
    "INSERT INTO utterances VALUES ('u1','knowledge','s1','x')",
    "INSERT INTO utterances VALUES ('u2','knowledge','s2','x')",
    "INSERT INTO utterances VALUES ('u3','knowledge','s3','x')",
    "INSERT INTO utterances VALUES ('u4','knowledge','s3','x')",
    "INSERT INTO utterances VALUES ('u5','knowledge','s3','x')",
    "INSERT INTO utterances VALUES ('u6','knowledge','s4','x')",
    "INSERT INTO utterances VALUES ('u7','knowledge','s5','x')",
    "INSERT INTO utterances VALUES ('u8','knowledge','s6','x')",
    "INSERT INTO utterances VALUES ('u10','knowledge','s12','x')",
    "INSERT INTO utterances VALUES ('u11','knowledge','s12','x')",
    "INSERT INTO utterances VALUES ('u12','knowledge','s13','x')",
    "INSERT INTO utterances VALUES ('u13','knowledge','s14','x')",
    "INSERT INTO utterances VALUES ('u9','other','s9','x')",
  ];
  const vocabulary = {
    affects: [
      { key: "accomplishment", label_ja: "達成感", valence: "positive", mda: "challenge", description: "やりきった" },
      { key: " calm ", label_ja: "  ", valence: 3, description: "落ち着き" },
      { key: "zeal" },
      { key: "" },
      { label_ja: "鍵なし" },
      { key: "Alpha" },
      { key: "beta_x" },
      { key: "beta1" },
    ],
  };
  const db = new Database(":memory:");
  for (const s of [...schema, ...inserts]) db.exec(s);
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "w1f-golden-"));
  fs.mkdirSync(path.join(tmp, "data", "affects"), { recursive: true });
  fs.writeFileSync(path.join(tmp, "data", "affects", "vocabulary.json"), JSON.stringify(vocabulary));
  const cwd = process.cwd();
  process.chdir(tmp);
  const strip = (v: Record<string, unknown>) => ({ ...v, generatedAt: 0 });
  const layers = {
    knowledge: strip(learningLayers.buildLearningLayerSnapshot(db, "knowledge", "knowledge") as never),
    knowledgeLimit: strip(learningLayers.buildLearningLayerSnapshot(db, "knowledge", "knowledge", { limit: 2 }) as never),
    games: strip(learningLayers.buildLearningLayerSnapshot(db, "knowledge", "games") as never),
    gamesLimit: strip(learningLayers.buildLearningLayerSnapshot(db, "knowledge", "games", { limit: 1.9, detailLimit: 0 }) as never),
    opinions: strip(learningLayers.buildLearningLayerSnapshot(db, "knowledge", "opinions") as never),
    opinionsNaN: strip(learningLayers.buildLearningLayerSnapshot(db, "knowledge", "opinions", { limit: NaN, detailLimit: 1 }) as never),
    otherWs: strip(learningLayers.buildLearningLayerSnapshot(db, "other", "games") as never),
  };
  fs.writeFileSync(path.join(tmp, "data", "affects", "vocabulary.json"), "{broken");
  const knowledgeBroken = strip(learningLayers.buildLearningLayerSnapshot(db, "knowledge", "knowledge") as never);
  process.chdir(cwd);
  fs.rmSync(tmp, { recursive: true, force: true });
  writeFixture("visualize.json", {
    schema,
    inserts,
    vocabulary,
    dataSources: dataSources.buildDataSourceSnapshot(db, "knowledge", 1000),
    dataSourcesOther: dataSources.buildDataSourceSnapshot(db, "nobody", 5),
    topics: strip(topicOpinions.buildTopicOpinionSnapshot(db, "knowledge") as never),
    topicsLimit: strip(topicOpinions.buildTopicOpinionSnapshot(db, "knowledge", { limit: 1, opinionsPerTopic: 1 }) as never),
    topicsHuge: strip(topicOpinions.buildTopicOpinionSnapshot(db, "knowledge", { limit: 1e9, opinionsPerTopic: -4 }) as never),
    layers: { ...layers, knowledgeBroken },
    normalize: ["games", "bad", "knowledge", "opinions", "", "Games"].map((v) => learningLayers.normalizeLearningLayer(v)),
  });
}

console.log(`wrote ${assetRecords.length} assets and fixtures to ${fixturesDir}`);
