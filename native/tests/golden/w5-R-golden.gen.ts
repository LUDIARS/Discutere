// w5-R 担当 (フォーラム入口・情報ゲートの実行・Web のフロー画面のルート・Slack の入口) の Node 実装の入出力を記録する。
// 実行: npx tsx native/tests/golden/w5-R-golden.gen.ts
// 出力: native/tests/fixtures/w5r/*.json
//   entry.json      … parseForumEntry / parseRoundsTurns / parseOpponents
//   flow_prep.json  … buildAutoCrawlSpec / buildLearningCrawl (config の env 違い)
//   synthetic.json  … normalizeSyntheticLearningOpinions
//   gate.json       … gateBeforeFlow (対象外の分岐と LLM 評価 1 回) / resolveDebatabilityGate
//   routes.json     … flowRoutes への要求列 (合成データ + 応答を決めた偽 LLM)。LLM 呼び出し列も記録する
//
// DB は一時ディレクトリ。data/ は読まない (cwd を一時ディレクトリへ移す)。外部サービスには繋がない。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import crypto from "node:crypto";

process.env.TZ = "UTC";
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "w5r-golden-"));
process.env.DISCUTERE_CONFIG = path.join(tmp, "no-config.json");
process.env.DATABASE_PATH = path.join(tmp, "flow.db");
process.env.DISCATIER_KUZU_PATH = path.join(tmp, "kg.kuzu");
process.env.DISCATIER_WORKSPACE = "ws";
process.env.DISCUTERE_PERSONA_ENGINE_DB = path.join(tmp, "pe.db");
for (const k of [
  "DISCUTERE_YOUTUBE_API_KEY",
  "DISCUTERE_REDDIT_CLIENT_ID",
  "DISCUTERE_REDDIT_CLIENT_SECRET",
  "NOTION_TOKEN",
  "LLM_BACKEND",
]) delete process.env[k];

const here = path.dirname(new URL(import.meta.url).pathname);
const nativeDir = path.join(here, "..", "..");
const fixturesDir = path.join(nativeDir, "tests", "fixtures", "w5r");
fs.mkdirSync(fixturesDir, { recursive: true });
const writeFixture = (name: string, value: unknown) =>
  fs.writeFileSync(path.join(fixturesDir, name), JSON.stringify(value, null, 2) + "\n");

const entry = await import("../../../src/flow/entry-discord.js");
const prep = await import("../../../src/flow/web/flow-prep.js");
const gateRunner = await import("../../../src/flow/information-gate-runner.js");
const routes = await import("../../../src/flow/web/routes.js");
const dp = await import("../../../src/flow/discussion-paper.js");
const { paperDraftToMarkdown } = await import("../../../src/flow/paper-markdown.js");
const { getFlowDb, _resetFlowDb } = await import("../../../src/flow/db/connection.js");
const { _resetConfig } = await import("../../../src/config.js");
const { createCore } = await import("../../../src/core/index.js");
const { Hono } = await import("hono");

process.chdir(tmp);

// ─── entry-discord ───────────────────────────────────────────────────────
const forumCases: Array<{ names: string[]; mapping?: unknown }> = [
  { names: ["改善", "機密"] },
  { names: ["壁打ち", "運用"] },
  { names: ["旧相談"], mapping: { flowKindTagNames: { sparring: ["旧相談"] } } },
  { names: ["議論"] },
  { names: ["面白さ"] },
  { names: [] },
  { names: ["  🔧 改善／議論 ", "内部", "内部タグ", "未来予測", "代理"] },
  { names: ["企画/議論", "開発"] },
  { names: ["相談A", "議論"], mapping: { flowKindTagNames: { improvement: ["相談"], discussion: ["雑談"] } } },
  { names: ["雑談", "改善"], mapping: { flowKindTagNames: { discussion: ["雑談"] }, flowTagNames: ["改善", "x"] } },
  { names: ["学習", "sparring", "discussion"] },
  { names: ["[機密] 運用"], mapping: { flowTagNames: ["運用", "機密"] } },
  { names: ["何か"], mapping: { flowTagNames: [""] } },
];
const roundsCases = [
  "ラウンド:5 ターン:8",
  "rounds=3 turns=6",
  "Round：4\nTurn = 2",
  "ラウンド ： 7",
  "xrounds=3 ターン:9",
  "rounds=x round=4",
  "ラウンド:abc ラウンド:6",
  "ラウンド:" + "9".repeat(400) + " rounds=2",
  "ROUNDS =12 TURNS= 01",
  "ラウンド　:　3",
  "rounds:3.5",
  "turn_s=3 turn=5",
  "",
  "ターン:" + "1".repeat(30),
];
const opponentCases = [
  "相手:ローグ好き太郎,ソシャゲ花子",
  "opponent=花子",
  "Opponent : A、B , ,C\n次の行",
  "相手:   ",
  "相手:\n次の行の名前",
  "相手: \n",
  "相手=,,、",
  "テーマ 相手なし",
  "相手ではない 相手：X",
  "OPPONENT: Y",
  "相手:　Z　",
  "",
];
writeFixture("entry.json", {
  forum: forumCases.map((c) => ({ ...c, out: entry.parseForumEntry(c.names, c.mapping as never) })),
  roundsTurns: roundsCases.map((text) => ({ text, out: entry.parseRoundsTurns(text) })),
  opponents: opponentCases.map((text) => ({ text, out: entry.parseOpponents(text) ?? null })),
});

// ─── flow-prep ───────────────────────────────────────────────────────────
const normAppId = (v: unknown) => (typeof v === "number" && Number.isFinite(v) ? Math.trunc(v) : undefined);
const normSpec = (s: Record<string, unknown> | null | undefined) => {
  if (!s) return null;
  const out = JSON.parse(JSON.stringify({ ...s, appId: normAppId(s.appId) }));
  return out;
};
const normLearning = (s: Record<string, unknown> | null | undefined) => {
  if (!s) return null;
  const by = s.specBySource as Record<string, Record<string, unknown>> | undefined;
  const fixed = by
    ? Object.fromEntries(Object.entries(by).map(([k, v]) => [k, { ...v, appId: normAppId(v.appId) }]))
    : undefined;
  return JSON.parse(JSON.stringify({ ...s, specBySource: fixed }));
};
const prepBodies: Array<Record<string, unknown>> = [
  {},
  { learningSource: "youtube", learningQuery: "  クエリ  " },
  { learningSource: "steam", learningAppId: "  12345 " },
  { learningSource: "steam", learningAppId: 678 },
  { learningSource: "steam", learningAppId: "abc" },
  { learningSource: "steam", learningAppId: "1e3" },
  { learningSource: "website", learningUrls: "https://a.example/x, https://b.example/y　https://c.example" },
  { learningSource: "website", learningUrls: ["https://a.example", "", "  ", 3, "https://b.example"] },
  { learningSource: "reddit", learningSubreddit: "  gamedev ", learningQuery: "" },
  { learningSource: "unknown" },
  { learningSource: "balanced", learningAppId: "42", learningSubreddit: "x", learningQuery: " q " },
  { learningBalanced: true },
  { learningBalanced: "true", learningQuery: "   " },
  { learningSource: 3, learningQuery: 4, learningUrls: "   " },
];
const prepEnvs: Array<Record<string, string | undefined>> = [
  {},
  { DISCUTERE_FLOW_AUTOCRAWL_ENABLED: "false" },
  { DISCUTERE_FLOW_AUTOCRAWL_SOURCE: "website", DISCUTERE_FLOW_AUTOCRAWL_SOURCES: "website,steam", DISCUTERE_FLOW_AUTOCRAWL_MAX_ITEMS: "35" },
  { DISCUTERE_FLOW_AUTOCRAWL_SOURCES: "niconico,youtube", DISCUTERE_FLOW_AUTOCRAWL_MAX_ITEMS: "7" },
  { DISCUTERE_FLOW_PAPER_REVIEW_ENABLED: "true", DISCUTERE_FLOW_PAPER_REVIEW_WEB_CANONICAL: "false" },
  { DISCUTERE_FLOW_PAPER_REVIEW_WEB_CANONICAL: "false" },
];
const prepOut: unknown[] = [];
for (const envs of prepEnvs) {
  for (const [k, v] of Object.entries(envs)) process.env[k] = v;
  _resetConfig();
  prepOut.push({
    env: envs,
    webPaperGateEnabled: prep.webPaperGateEnabled(),
    cases: prepBodies.map((body) => ({
      body,
      autoCrawl: normSpec(prep.buildAutoCrawlSpec(body) as never),
      learning: normLearning(prep.buildLearningCrawl({ ...body, youtubeApiKey: null }) as never),
      learningWithKey: normLearning(prep.buildLearningCrawl({ ...body, youtubeApiKey: "fake-key" }) as never),
    })),
  });
  for (const k of Object.keys(envs)) delete process.env[k];
  _resetConfig();
}
writeFixture("flow_prep.json", prepOut);

// ─── synthetic voices ────────────────────────────────────────────────────
const syntheticCases: unknown[][] = [
  [
    { segment: "初心者", polarity: "negative", concern: "チュートリアル理解", content: "[synthetic] 何をすれば強くなるのか最初に分からず、説明を飛ばしたら戻れないのがつらい。" },
    { segment: "初心者", polarity: "negative", concern: "チュートリアル理解", content: "[synthetic] 最初に何をすれば強くなるのか分からず、説明を飛ばすと戻れないのがつらい。" },
    { segment: "復帰者", polarity: "mixed", concern: "UI変化", content: "[synthetic] 昔より便利そうだが、復帰直後は画面の情報量が多くて迷う。" },
    { segment: "上級者", polarity: "positive", concern: "序盤テンポ", content: "[synthetic] 既存プレイヤーには短い導線だけで十分で、すぐクエストへ行ける方が良い。" },
    { segment: "無課金", polarity: "negative", concern: "ガチャ導線", content: "[synthetic] 序盤からガチャと育成素材の説明が絡むと、課金前提に見えて身構える。" },
    { segment: "友達紹介", polarity: "mixed", concern: "協力プレイ", content: "[synthetic] 友達に誘われた人にはマルチの楽しさを早く見せたいが、操作説明が長いと離れそう。" },
  ],
  [
    "  [SYNTHETIC]   素の文字列の声   です  ",
    { content: "[仮説 Synthetic 生成] 括弧つきラベル", persona: "  熟練   プレイヤー  ", issue: null, topic: "報酬", sentiment: "賛成寄り" },
    { content: "  ", segment: "空" },
    { content: 5 },
    null,
    ["配列"],
    { content: "不満が多い声", polarity: "NEGATIVE ", segment: 3, concern: "x".repeat(50) },
    { content: "😀😀 絵文字の声 𠮷野家", segment: "Ａｂｃ", concern: "絵文字" },
    { content: "😀😀 絵文字の声 𠮷野家!", segment: "abc", concern: "絵文字2" },
    { content: "あ".repeat(600), polarity: "否定的" },
    { content: "短い", segment: "s1", concern: "c1", polarity: "positive" },
    { content: "短い", segment: "s2", concern: "c2", polarity: "positive" },
    { content: "ab", segment: "s3", concern: "c3" },
  ],
  [],
];
writeFixture(
  "synthetic.json",
  syntheticCases.map((items) => ({ items, out: routes.normalizeSyntheticLearningOpinions(items, 1000) }))
);

// ─── 偽 LLM (system の印で応答を決める) ───────────────────────────────────
type Call = { system?: string; prompt: string; model?: string; maxTokens?: number };
const calls: Call[] = [];
const ROUTES: Array<[string, string]> = [
  ["情報が足りているか", '{"density":"rich","covered":["肯定","否定"],"gaps":[]}'],
  ["議論設計者", '["周回報酬の是非","課金圧の影響"]'],
  ["両方が根拠を張れるか", '[{"index":1,"armable":"both"},{"index":"2","armable":"pro-only"},{"index":9,"armable":"bogus"}]'],
  ["渡された markdown ブロック", '前置き {"proposed":"# 議題\\n改稿後の議題","rationale":"整えた"} 後置き'],
  ["根拠段落", "  集めた声によると周回は好評。  "],
  ["You review a Japanese discussion paper", '[{"title":" 論点補強 ","reason":"弱い","suggestedChange":"{{具体例}} を追記"},{"title":"","suggestedChange":"x"},"bad",{"title":"t2","suggestedChange":"s2"}]'],
  ["You check whether you have enough game mechanics", 'x {"ok":"no","confidence":"medium","summary":"  概ね把握  ","knownMechanics":["周回"," ","強化",3],"missingQuestions":["報酬量は?"]} y'],
  ["hypothesis-labeled player feedback", "[]"],
];
const llm = {
  async invoke(args: { system?: string; prompt: string; model?: string; maxTokens?: number }) {
    const c: Call = { prompt: args.prompt };
    if (args.system !== undefined) c.system = args.system;
    if (args.model !== undefined) c.model = args.model;
    if (args.maxTokens !== undefined) c.maxTokens = args.maxTokens;
    calls.push(c);
    const sys = args.system ?? "";
    for (const [mark, text] of ROUTES) if (sys.includes(mark)) return { ok: true as const, text };
    return { ok: false as const, error: "no route" };
  },
};
const VOICES = [
  { content: "周回が楽しい。報酬も十分。", source: "youtube", sourceUrl: "https://example.invalid/1" },
  { content: "課金しないと厳しい。" + "長".repeat(130), source: "steam" },
  { content: "テンポが良い", source: "niconico" },
];
const voiceCalls: Array<{ terms: string[]; limit: number }> = [];
const listExternalVoices = (terms: string[], limit: number) => {
  voiceCalls.push({ terms, limit });
  return VOICES.slice(0, limit);
};

// ─── information-gate-runner ─────────────────────────────────────────────
const gateCases: unknown[] = [];
const runGate = async (label: string, kind: string, tags: string[], withCore: boolean, envs: Record<string, string> = {}) => {
  for (const [k, v] of Object.entries(envs)) process.env[k] = v;
  _resetConfig();
  calls.length = 0;
  voiceCalls.length = 0;
  const logs: string[] = [];
  const res = await gateRunner.gateBeforeFlow({
    kind: kind as never,
    theme: "周回ゲームの報酬",
    tags: tags as never,
    llm: llm as never,
    openCore: withCore ? () => createCore(path.join(tmp, "kg.kuzu")) : undefined,
    workspaceId: "ws",
    listExternalVoices,
    sessionId: "gate-sess",
    log: (m) => logs.push(`log:${m}`),
    warn: (m) => logs.push(`warn:${m}`),
    youtubeApiKey: null,
  });
  gateCases.push({ label, kind, tags, withCore, env: envs, result: res, calls: [...calls], voiceCalls: [...voiceCalls], logs });
  for (const k of Object.keys(envs)) delete process.env[k];
  _resetConfig();
};
await runGate("disabled", "discussion", [], true, { DISCUTERE_FLOW_INFOGATE_ENABLED: "false" });
await runGate("learning-kind", "learning", [], true);
await runGate("no-core", "discussion", [], false);
await runGate("no-sources", "improvement", [], true, { DISCUTERE_FLOW_AUTOCRAWL_SOURCES: "website,steam" });
await runGate("rich", "discussion", ["運用"], true, { DISCUTERE_FLOW_INFOGATE_MODEL: "gate-model", DISCUTERE_FLOW_INFOGATE_MIN_DENSITY: "bogus" });
const costRows = getFlowDb()
  .prepare("SELECT flow, session_id, location FROM llm_call_log ORDER BY id")
  .all();
const debGate: unknown[] = [];
for (const [kind, envs] of [
  ["discussion", {}],
  ["improvement", { DISCUTERE_FLOW_DEBATABILITY_MIN_ARMABLE_ISSUES: "4" }],
  ["learning", {}],
  ["discussion", { DISCUTERE_FLOW_DEBATABILITY_ENABLED: "false" }],
] as Array<[string, Record<string, string>]>) {
  for (const [k, v] of Object.entries(envs)) process.env[k] = v;
  _resetConfig();
  const g = gateRunner.resolveDebatabilityGate({ kind: kind as never, sessionId: "s", llm: llm as never });
  debGate.push({ kind, env: envs, out: g ? { minArmableIssues: g.minArmableIssues } : null });
  for (const k of Object.keys(envs)) delete process.env[k];
  _resetConfig();
}
writeFixture("gate.json", { cases: gateCases, costRows, debatabilityGate: debGate });
getFlowDb().prepare("DELETE FROM llm_call_log").run();

// ─── routes ──────────────────────────────────────────────────────────────
_resetConfig();
routes._resetFlowWeb();
routes.setFlowWebDeps({ workspaceId: "ws", llm: llm as never, gamesDir: path.join(tmp, "no-games"), listExternalVoices });
const app = new Hono();
app.route("/", routes.flowRoutes);

const simBody = paperDraftToMarkdown({
  theme: "仮想ユーザ補填テーマ",
  gameTitle: "Test Game",
  discussionTheme: "仮想ユーザ補填テーマ",
  discussionContent: "既存の実ユーザ声は肯定に偏っている。",
  mechanicsContext:
    "基本ループはステージ周回、報酬獲得、強化、再挑戦で構成される。失敗時の損失、報酬密度、待ち時間、課金圧がユーザ反応を左右する。",
  themeSupplement: "",
  tags: [],
  supplement: "",
  mechanics: [{ name: "周回", description: "報酬獲得と強化を繰り返す" }],
});
const draftBody = paperDraftToMarkdown({
  theme: "下書き議題",
  gameTitle: "下書きゲーム",
  discussionTheme: "下書き議題",
  discussionContent: "周回の報酬を見直したい。",
  mechanicsContext: "",
  themeSupplement: "",
  tags: ["運用"],
  supplement: "",
  mechanics: [{ name: "周回", description: "繰り返し遊ぶ" }],
});
const setup: Array<Record<string, unknown>> = [
  { op: "persistPaper", flow: "discussion", paper: { sessionId: "list-sess-1", theme: "一覧テスト議題", tags: [], mechanics: [], supplement: "", bodyMd: "# 議題\n一覧テスト議題" } },
  { op: "persistPaper", flow: "discussion", paper: { sessionId: "done-sess-1", theme: "収束前の議題", tags: [], mechanics: [], supplement: "", bodyMd: "# 議題\n収束前の議題" } },
  { op: "sql", sql: "INSERT INTO flow_conclusion (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at) VALUES ('done-sess-1', 'done-sess-1', '【収束】収束時のタイトル\n詳細本文', '[]', '[\"u-top\", 3]', 1, 5000)" },
  { op: "titleCache", sessionId: "done-sess-1", raw: "【収束】収束時のタイトル\n詳細本文", source: "conclusion" },
  { op: "persistDraftPaper", flow: "discussion", paper: { sessionId: "draft-sess-1", theme: "下書き議題", tags: ["運用"], mechanics: [{ name: "周回", description: "繰り返し遊ぶ" }], supplement: "", bodyMd: draftBody } },
  { op: "reviewInfo", sessionId: "draft-sess-1", info: { voiceCount: 0, countCapped: false, samples: [], fixSuggestions: [{ title: "論点補強", reason: "対立軸が弱い", suggestedChange: "{{具体例}} をもとに、賛成側と反対側の争点を追記する。" }, { title: "  ", reason: "r", suggestedChange: "二つ目の提案" }] } },
  { op: "persistDraftPaper", flow: "discussion", paper: { sessionId: "sim-sess-1", theme: "仮想ユーザ補填テーマ", tags: [], mechanics: [{ name: "周回", description: "報酬獲得と強化を繰り返す" }], supplement: "", bodyMd: simBody } },
  {
    op: "reviewInfo",
    sessionId: "sim-sess-1",
    info: {
      voiceCount: 50,
      countCapped: true,
      samples: [],
      debatability: {
        issues: ["争点A", "争点B", "争点C"],
        armability: [
          { issue: "争点A", armable: "pro-only" },
          { issue: "争点B", armable: "pro-only" },
          { issue: "争点C", armable: "neither" },
        ],
        armableBothCount: 0,
        minArmableIssues: 2,
        evidence: { voiceCount: 50, positive: 50, negative: 0, neutral: 0, polaritySkew: 1, dominantSource: { name: "learning", share: 1 } },
        debatable: false,
        degraded: false,
        recommendation: { flow: "learning", reason: "材料不足" },
        message: "議論適性: 低",
      },
    },
  },
  { op: "persistDraftPaper", flow: "improvement", paper: { sessionId: "conf-sess-1", theme: "機密の議題", tags: ["機密"], mechanics: [], supplement: "", bodyMd: "# 議題\n機密の議題\n\n本文の段落" } },
  { op: "reviewInfo", sessionId: "conf-sess-1", info: { voiceCount: 2, countCapped: false, samples: [], understanding: { ok: false, rationale: "不足", missingQuestions: ["基本ループ"] } } },
  { op: "persistPaper", flow: "discussion", paper: { sessionId: "mark-sess-1", theme: "マーク議題", tags: [], mechanics: [], supplement: "", bodyMd: "# 議題\nマーク議題" } },
  {
    op: "sql",
    sql: "INSERT INTO flow_utterance (id, session_id, paper_id, round, turn, persona_id, persona_name, role, stance, possession_name, text, is_error, created_at) VALUES ('u-voted', 'mark-sess-1', (SELECT id FROM discussion_paper WHERE session_id='mark-sess-1'), 1, 1, 'p1', '論者A', 'opinion', 'opinion', NULL, '意見A', 0, 100), ('u-other', 'mark-sess-1', (SELECT id FROM discussion_paper WHERE session_id='mark-sess-1'), 1, 2, 'p2', '論者B', 'debater', 'pro', '憑依X', '意見B', 1, 101), ('u-r2', 'mark-sess-1', (SELECT id FROM discussion_paper WHERE session_id='mark-sess-1'), 2, 1, 'p3', '論者C', 'facilitator', 'neutral', NULL, '意見C', 0, 102)",
  },
  { op: "sql", sql: "INSERT INTO vote (session_id, round, voter_index, chosen_utterance_id, created_at) VALUES ('mark-sess-1', 1, 0, 'u-voted', 200), ('mark-sess-1', 1, 1, 'u-voted', 201), ('mark-sess-1', 1, 2, 'u-other', 202), ('mark-sess-1', 2, 0, NULL, 203)" },
  { op: "sql", sql: "INSERT INTO discussion_paper_round (paper_id, round, summary, aufhebung_json, created_at) VALUES ((SELECT id FROM discussion_paper WHERE session_id='mark-sess-1'), 1, 'まとめ', '[\"止揚案\", 7]', 202), ((SELECT id FROM discussion_paper WHERE session_id='mark-sess-1'), 2, 'まとめ2', 'not json', 203)" },
  { op: "sql", sql: "INSERT INTO flow_conclusion (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at) VALUES ('mark-sess-1', 'p', '結論です', '[]', '[\"u-r2\"]', 0, 300)" },
  // 並び順を決めるため created_at を固定する (同じ ms に入ると順が揺れる)。
  { op: "sql", sql: "UPDATE discussion_paper SET created_at = CASE session_id WHEN 'list-sess-1' THEN 1001 WHEN 'done-sess-1' THEN 1002 WHEN 'draft-sess-1' THEN 1003 WHEN 'sim-sess-1' THEN 1004 WHEN 'conf-sess-1' THEN 1005 WHEN 'mark-sess-1' THEN 1006 ELSE created_at END, updated_at = 2000" },
];
for (const s of setup) {
  if (s.op === "persistPaper") dp.persistPaper(s.paper as never, s.flow as never);
  else if (s.op === "persistDraftPaper") dp.persistDraftPaper(s.paper as never, s.flow as never);
  else if (s.op === "sql") getFlowDb().exec(s.sql as string);
  else if (s.op === "titleCache") dp.upsertDiscussionTitleCache(s.sessionId as string, s.raw as string, s.source as never);
  else if (s.op === "reviewInfo") dp.setPaperReviewInfo(s.sessionId as string, s.info as never);
}

type Req = { method: string; url: string; body?: unknown; rawBody?: string };
const reqs: Req[] = [
  { method: "POST", url: "/api/flow/start", body: { theme: "テーマ" } },
  { method: "POST", url: "/api/flow/nope/paper/edit", body: { instruction: "全体を調整して" } },
  { method: "POST", url: "/api/flow/start", body: { theme: "", flow: "discussion" } },
  { method: "POST", url: "/api/flow/start", body: { theme: "  ", flow: "議論", gameTitle: "", discussionTheme: "", discussionContent: "内容だけ" } },
  { method: "POST", url: "/api/flow/start", body: { flow: "改善", discussionTheme: "テーマ" } },
  { method: "POST", url: "/api/flow/start", body: { theme: "壁打ちテーマ", flow: "壁打ち", tags: [] } },
  { method: "POST", url: "/api/flow/start", rawBody: "{broken" },
  { method: "POST", url: "/api/flow/nope/say", body: { text: "x" } },
  { method: "GET", url: "/api/flow/sessions" },
  { method: "GET", url: "/api/flow/sessions?state=draft" },
  { method: "GET", url: "/api/flow/sessions?state=live&scope=ai" },
  { method: "GET", url: "/api/flow/sessions?state=concluded" },
  { method: "GET", url: "/api/flow/sessions?scope=chat" },
  { method: "GET", url: "/api/flow/sessions?limit=1&offset=0" },
  { method: "GET", url: "/api/flow/sessions?limit=1&offset=1" },
  { method: "GET", url: "/api/flow/sessions?limit=0&offset=-5&state=bogus" },
  { method: "GET", url: "/api/flow/sessions?limit=999&offset=abc" },
  { method: "GET", url: "/api/flow/sessions?limit=2.9x&offset=+1" },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/form/apply", body: { discussionTheme: "x" } },
  { method: "GET", url: "/api/flow/draft-sess-1/paper" },
  { method: "GET", url: "/api/flow/list-sess-1/paper" },
  { method: "GET", url: "/api/flow/nope-sess/paper" },
  { method: "GET", url: "/api/flow/sim-sess-1/paper" },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/fix-suggestion/apply", body: { suggestionIndex: 5 } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/fix-suggestion/apply", body: { suggestionIndex: "0" } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/fix-suggestion/apply", body: { suggestionIndex: 1, text: "x".repeat(8001) } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/fix-suggestion/apply", body: { suggestionIndex: 0, text: "周回報酬の緩和案をもとに、賛成側と反対側の争点を追記する。" } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/fix-suggestion/apply", body: { suggestionIndex: 0 } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/fix-suggestion/apply", body: { suggestionIndex: 1 } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/form/apply", body: { gameTitle: "", discussionTheme: "新テーマ" } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/form/apply", body: { gameTitle: " 新ゲーム ", discussionTheme: " 新テーマ ", themeSupplement: "補足", tags: ["機密", "bogus", "運用", "機密", 3], rounds: "4", turnsPerRound: 99, issues: "1. 論点一\r\n- 論点二\n\n*  論点三\n12)論点四\n  " } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/form/apply", body: { tags: "notarray", issues: ["  - 配列の論点 ", 5, ""], rounds: null } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/mechanics/check", body: {} },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/form/apply", body: { tags: [] } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/debatability/check", body: {} },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/block/review", body: {} },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/block/review", body: { blockId: "b0", instruction: "短く" } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/block/review", body: { blockId: "b999" } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/block/apply", body: { blockId: "b1", newText: "手で直した段落", summary: "  " } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/block/apply", body: { newText: "x" } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/crawl", body: { blockId: "b0" } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/crawl", body: { query: " 明示クエリ ", insert: true } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/crawl", body: { blockId: "b1", insert: true } },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/revert", body: {} },
  { method: "POST", url: "/api/flow/draft-sess-1/paper/revert", body: {} },
  { method: "GET", url: "/api/flow/draft-sess-1/paper" },
  { method: "GET", url: "/api/flow/conf-sess-1/paper" },
  { method: "POST", url: "/api/flow/conf-sess-1/paper/crawl", body: {} },
  { method: "POST", url: "/api/flow/conf-sess-1/paper/debatability/check", body: {} },
  { method: "POST", url: "/api/flow/conf-sess-1/paper/mechanics/check", body: {} },
  { method: "POST", url: "/api/flow/nope/paper/approve", body: {} },
  { method: "POST", url: "/api/flow/nope/paper/revert", body: {} },
  { method: "GET", url: "/api/flow/mark-sess-1/status?since=0" },
  { method: "GET", url: "/api/flow/mark-sess-1/status?since=100" },
  { method: "GET", url: "/api/flow/nope/status" },
  { method: "DELETE", url: "/api/flow/draft-sess-1" },
  { method: "DELETE", url: "/api/flow/draft-sess-1" },
  { method: "GET", url: "/api/flow/sessions" },
  { method: "GET", url: "/api/flow/draft-sess-1/paper" },
];
const records: unknown[] = [];
let revisions: unknown = null;
let reviewInfoAfter: unknown = null;
for (const r of reqs) {
  if (r.method === "DELETE" && revisions === null) {
    // 削除で消える前に版履歴と保存した情報を記録する。
    revisions = getFlowDb()
      .prepare("SELECT session_id, rev, body_md, change_summary, origin FROM discussion_paper_revision ORDER BY session_id, rev")
      .all();
    reviewInfoAfter = dp.getPaperReviewInfo("draft-sess-1") ?? null;
  }
  calls.length = 0;
  voiceCalls.length = 0;
  const init: RequestInit = { method: r.method };
  if (r.body !== undefined || r.rawBody !== undefined) {
    init.headers = { "content-type": "application/json" };
    init.body = r.rawBody ?? JSON.stringify(r.body);
  }
  const res = await app.request(r.url, init);
  const text = await res.text();
  let body: unknown = text;
  try {
    body = JSON.parse(text);
  } catch {
    body = text;
  }
  records.push({ req: r, status: res.status, body, calls: [...calls], voiceCalls: [...voiceCalls] });
}
const page = await app.request("/flow");
const pageText = await page.text();
writeFixture("routes.json", {
  setup,
  requests: records,
  page: { status: page.status, contentType: page.headers.get("content-type"), sha256: crypto.createHash("sha256").update(pageText).digest("hex") },
  revisions,
  reviewInfoAfter,
});

routes._resetFlowWeb();
_resetFlowDb();
process.chdir(nativeDir);
fs.rmSync(tmp, { recursive: true, force: true });
console.log("w5-R golden: written to", fixturesDir);
