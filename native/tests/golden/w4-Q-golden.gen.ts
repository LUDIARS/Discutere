// w4-Q 担当 (残りの API ルート・Discord のコマンドルータ・フォーラム監視・発話の投入口) の Node 実装の入出力を記録する。
// 実行: npx tsx native/tests/golden/w4-Q-golden.gen.ts
// 出力:
//   native/web/api/{consensus,chat,learning}.html … 画面 (Node が返す文字列をそのまま。C++ は移植しない)
//   native/tests/fixtures/w4q/*.json             … C++ テストが同じ出力を返すかを確かめる記録
//
// DB は合成データ (setup の SQL) を一時ディレクトリに作る。C++ テストは同じ SQL を自分の DB に流して同じ要求を送る。
// 時刻は TZ=UTC で固定 (toLocaleString("ja-JP") の golden のため)。data/ は読まない (cwd を一時ディレクトリへ移す)。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import crypto from "node:crypto";

process.env.TZ = "UTC";
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "w4q-golden-"));
process.env.DISCUTERE_CONFIG = path.join(tmp, "no-config.json");
process.env.DATABASE_PATH = path.join(tmp, "flow.db");
process.env.DISCATIER_KUZU_PATH = path.join(tmp, "kg.kuzu");
process.env.DISCATIER_WORKSPACE = "ws";
process.env.DISCUTERE_PERSONA_ENGINE_DB = path.join(tmp, "pe.db");
delete process.env.DISCUTERE_YOUTUBE_API_KEY;

const here = path.dirname(new URL(import.meta.url).pathname);
const nativeDir = path.join(here, "..", "..");
const fixturesDir = path.join(nativeDir, "tests", "fixtures", "w4q");
fs.mkdirSync(fixturesDir, { recursive: true });
const writeFixture = (name: string, value: unknown) =>
  fs.writeFileSync(path.join(fixturesDir, name), JSON.stringify(value, null, 2) + "\n");

const { Hono } = await import("hono");
const auth = await import("../../../src/middleware/auth.js");
const { consensusRoutes } = await import("../../../src/api/consensus-routes.js");
const { webChatRoutes, setWebChatDeps } = await import("../../../src/api/web-chat-routes.js");
const { learningViewRoutes, HTML: LEARNING_HTML } = await import("../../../src/api/learning-view-routes.js");
const { createCore } = await import("../../../src/core/index.js");
const { submitMessage } = await import("../../../src/core/projection/message-input.js");
const { getFlowDb } = await import("../../../src/flow/db/connection.js");
const router = await import("../../../src/discord-hook/command-router.js");
const forum = await import("../../../src/discord-hook/forum-monitor.js");

process.chdir(tmp);

const ADMIN = { "X-User-Id": "golden", "X-User-Role": "admin" };

// ─── 画面 ────────────────────────────────────────────────────────────────
const apiApp = new Hono();
apiApp.use("*", auth.userContext());
apiApp.route("/api", consensusRoutes);
const assets: Array<{ name: string; html: string }> = [
  { name: "api/consensus.html", html: await (await apiApp.request("/api/admin/consensus", { headers: ADMIN })).text() },
  { name: "api/chat.html", html: await (await webChatRoutes.request("/chat")).text() },
  { name: "api/learning.html", html: LEARNING_HTML },
];
const assetRecords = assets.map(({ name, html }) => {
  const file = path.join(nativeDir, "web", name);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, html);
  return { name, bytes: Buffer.byteLength(html), sha256: crypto.createHash("sha256").update(html).digest("hex") };
});
writeFixture("web_assets.json", assetRecords);

// ─── 合成データ ──────────────────────────────────────────────────────────
const kgSetup = [
  `CREATE TABLE IF NOT EXISTS consensus_scores (utterance_id TEXT PRIMARY KEY, agree INTEGER NOT NULL, score INTEGER NOT NULL, reasoning TEXT, created_at INTEGER NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS opinion_scores (target_id TEXT PRIMARY KEY, target_kind TEXT NOT NULL, score INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL)`,
  `INSERT INTO opinion_scores (target_id, target_kind, score, updated_at) VALUES ('u6', 'utterance', 3, 1)`,
  `CREATE TABLE IF NOT EXISTS aufhebung_stock (id TEXT PRIMARY KEY, gap_id TEXT NOT NULL, summary TEXT NOT NULL, created_at INTEGER NOT NULL)`,
  `INSERT INTO aufhebung_stock (id, gap_id, summary, created_at) VALUES ('a1', 'g1', '止揚A\n\n\nつづき', 10)`,
  `INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('g1', 'ws', 'ローグライトの周回', 'converged', 1, 300)`,
  `INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('g2', 'ws', '進行中の議論', NULL, 1, 200)`,
  `INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('g3', 'other', '別WS', 'closed', 1, 400)`,
  `INSERT INTO design_gaps (id, workspace_id, title, status, created_at, updated_at) VALUES ('g4', 'ws', 'フォーラム議論', 'open', 1, 100)`,
  `INSERT INTO sessions (id, workspace_id, title, started_at, scene, created_at, updated_at) VALUES ('s1', 'ws', 'discussion-of-gap:g1', 10, 'discord:G1/T1', 1, 1)`,
  `INSERT INTO sessions (id, workspace_id, title, started_at, scene, created_at, updated_at) VALUES ('s2', 'ws', 'discussion-of-gap:g2', 20, 'web:lobby', 1, 1)`,
  `INSERT INTO sessions (id, workspace_id, title, started_at, scene, created_at, updated_at) VALUES ('s3', 'other', 'discussion-of-gap:g3', 30, NULL, 1, 1)`,
  `INSERT INTO sessions (id, workspace_id, title, started_at, scene, created_at, updated_at) VALUES ('s4', 'ws', 'discussion-of-gap:g4', 40, 'discord:G1/T4', 1, 1)`,
  `INSERT INTO sessions (id, workspace_id, title, started_at, scene, created_at, updated_at) VALUES ('w1', 'ws', 'web-session:lobby', 5, 'web:lobby', 1, 1)`,
  `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('u1', 'ws', 's1', 'persona:陽介', '周回前提なら飽きが課題', 100, 1, 1)`,
  `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('u2', 'ws', 's1', 'discord:123', 'ビルド分岐で多様性を出せる', 110, 1, 1)`,
  `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('u3', 'ws', 's1', 'ext:youtube:abc', '外部の声', 120, 1, 1)`,
  `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('u4', 'ws', 's1', 'persona:facilitator', '進行役', 130, 1, 1)`,
  `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('u5', 'ws', 's1', 'persona:ミナ', '【収束】周回はビルド多様性で飽きを防ぐ\n\n\n詳細', 140, 1, 1)`,
  `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('u6', 'ws', 's1', 'persona:ミナ', '報酬設計も\n\n\n大事', 150, 1, 1)`,
  `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('u7', 'ws', 's1', NULL, '匿名', 160, 1, 1)`,
  `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('wu1', 'ws', 'w1', '太郎', 'ローグライトの面白さの核は何か', 1000, 1, 1)`,
  `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('wu2', 'ws', 's2', 'persona:p-a', '探索の手触りが核', 2000, 1, 1)`,
  `INSERT INTO utterances (id, workspace_id, session_id, speaker_id, raw_content, posted_at, created_at, updated_at) VALUES ('wu3', 'ws', 's2', 'persona:facilitator', '論点を整理します', 3000, 1, 1)`,
  `INSERT INTO consensus_scores (utterance_id, agree, score, reasoning, created_at) VALUES ('u1', 1, 80, '多くが同意', 1)`,
  `INSERT INTO consensus_scores (utterance_id, agree, score, reasoning, created_at) VALUES ('u2', 0, 30, NULL, 1)`,
];
const flowSetup = [
  `INSERT INTO discussion_paper (id, flow, session_id, theme, tags_json, mechanics_json, supplement, created_at, updated_at, status, discussion_no) VALUES ('p1', 'discussion', 'fs-1', 'ローグライトの周回設計', '["開発"]', '[{"name":"ビルド選択","description":"周回ごとに構成を変える"}]', '', 1700000000000, 1700000000000, 'started', 1)`,
  `INSERT INTO discussion_paper (id, flow, session_id, theme, tags_json, mechanics_json, supplement, created_at, updated_at, status, discussion_no) VALUES ('p2', 'sparring', 'fs-2', '壁打ちの議題', '[]', '[]', '', 1700000100000, 1700000100000, 'started', 2)`,
  `INSERT INTO discussion_paper (id, flow, session_id, theme, tags_json, mechanics_json, supplement, created_at, updated_at, status, discussion_no) VALUES ('p3', 'discussion', 'fs-3', '', '[]', '[]', '', 1700000200000, 1700000200000, 'draft', NULL)`,
  `INSERT INTO discussion_paper (id, flow, session_id, theme, tags_json, mechanics_json, supplement, created_at, updated_at, status, discussion_no) VALUES ('p4', 'improvement', 'fs-4', '${"とても長い議題".repeat(12)}', '[]', '[]', '', 1700000300000, 1700000300000, 'started', 4)`,
  `INSERT INTO flow_conclusion (session_id, paper_id, summary, aufhebung_json, top_utterance_ids_json, concluded, created_at) VALUES ('fs-1', 'p1', '【収束】周回はビルド多様性で飽きを防ぐ', '["難度と報酬の対応を保つ"]', '["fu1"]', 1, 1700000050000)`,
  `INSERT INTO flow_utterance (id, session_id, paper_id, round, turn, persona_id, persona_name, role, stance, text, is_error, possession_name, created_at) VALUES ('fu1', 'fs-1', 'p1', 1, 1, 'a', '陽介', 'debater', 'pro', 'ビルド分岐で飽きを防ぐ', 0, NULL, 1700000010000)`,
  `INSERT INTO flow_utterance (id, session_id, paper_id, round, turn, persona_id, persona_name, role, stance, text, is_error, possession_name, created_at) VALUES ('fu2', 'fs-2', 'p2', 1, 1, 'b', 'ミナ', 'opinion', 'opinion', '壁打ち', 0, NULL, 1700000110000)`,
];

function freshDbs() {
  for (const f of fs.readdirSync(tmp)) fs.rmSync(path.join(tmp, f), { recursive: true, force: true });
  const core = createCore(process.env.DISCATIER_KUZU_PATH!, path.join(tmp, "events.jsonl"));
  for (const s of kgSetup) core.client.raw.exec(s);
  core.close();
}

// ─── ルートの応答 ─────────────────────────────────────────────────────────
const { _resetFlowDb } = await import("../../../src/flow/db/connection.js");
freshDbs();
_resetFlowDb();
for (const s of flowSetup) getFlowDb().exec(s);

const routeCases: Array<{ name: string; method: string; target: string; headers?: Record<string, string>; app: "api" | "root" }> = [
  { name: "consensus gaps (admin)", method: "GET", target: "/api/admin/consensus/gaps", headers: ADMIN, app: "api" },
  { name: "consensus opinions g1", method: "GET", target: "/api/admin/consensus/g1", headers: ADMIN, app: "api" },
  { name: "consensus opinions missing", method: "GET", target: "/api/admin/consensus/none", headers: ADMIN, app: "api" },
  { name: "consensus html non-admin", method: "GET", target: "/api/admin/consensus", headers: { "X-User-Id": "u", "X-User-Role": "general" }, app: "api" },
  { name: "consensus gaps no user", method: "GET", target: "/api/admin/consensus/gaps", headers: { "X-User-Role": "admin", "X-User-Id": "" }, app: "api" },
  { name: "chat messages lobby", method: "GET", target: "/api/chat/lobby/messages", app: "root" },
  { name: "chat messages since", method: "GET", target: "/api/chat/lobby/messages?since=1500", app: "root" },
  { name: "chat messages sanitized room", method: "GET", target: "/api/chat/lo%20bby!!/messages?since=abc", app: "root" },
  { name: "chat sessions", method: "GET", target: "/api/chat/sessions", app: "root" },
  { name: "chat sessions paged", method: "GET", target: "/api/chat/sessions?state=live&limit=0&offset=-3", app: "root" },
  { name: "chat sessions bad state", method: "GET", target: "/api/chat/sessions?state=weird&limit=500", app: "root" },
  { name: "learning gap (no cache)", method: "GET", target: "/learning/gap", app: "root" },
  { name: "learning gap detail (no cache)", method: "GET", target: "/learning/gap/detail?slug=x", app: "root" },
  { name: "learning sources (no cache)", method: "GET", target: "/learning/sources", app: "root" },
  { name: "learning conclusion flow", method: "GET", target: "/learning/conclusion?gap=flow:fs-1", app: "root" },
  { name: "learning conclusion gap", method: "GET", target: "/learning/conclusion?gap=g1", app: "root" },
  { name: "learning conclusion missing", method: "GET", target: "/learning/conclusion?gap=nope", app: "root" },
  { name: "learning export flow md", method: "GET", target: "/learning/conclusion/export?gap=flow:fs-1", app: "root" },
  { name: "learning export missing", method: "GET", target: "/learning/conclusion/export?gap=flow:nope", app: "root" },
  { name: "learning data knowledge", method: "GET", target: "/learning/data?layer=opinions&limit=5", app: "root" },
  // 最後に一覧 (ensureConclusionCacheFresh がキャッシュを書くので後の要求の前提を変える)
  { name: "learning conclusions", method: "GET", target: "/learning/conclusions", app: "root" },
  { name: "learning conclusions limit 1", method: "GET", target: "/learning/conclusions?limit=1", app: "root" },
];
const rootApp = new Hono();
rootApp.route("/", webChatRoutes);
rootApp.route("/", learningViewRoutes);
setWebChatDeps({ workspaceId: "ws" });
const routes: unknown[] = [];
for (const rc of routeCases) {
  const app = rc.app === "api" ? apiApp : rootApp;
  const res = await app.request(rc.target, { method: rc.method, headers: rc.headers ?? {} });
  const text = await res.text();
  let body: unknown = text;
  if ((res.headers.get("content-type") ?? "").includes("application/json")) body = JSON.parse(text);
  else if ((res.headers.get("content-type") ?? "").includes("text/html")) body = { sha256: crypto.createHash("sha256").update(text).digest("hex") };
  routes.push({
    ...rc,
    status: res.status,
    contentType: res.headers.get("content-type"),
    contentDisposition: res.headers.get("content-disposition"),
    body,
  });
}
// フォールバック (キャッシュ空・新フロー結論なし) の live マージ: flow DB を空にして KG の旧議論だけを出す。
freshDbs();
_resetFlowDb();
getFlowDb();
const fallback: unknown[] = [];
for (const target of ["/learning/conclusions", "/learning/conclusions?limit=-1", "/learning/conclusions?limit=abc"]) {
  _resetFlowDb();
  fs.rmSync(process.env.DATABASE_PATH!, { force: true });
  const res = await rootApp.request(target);
  const text = await res.text();
  fallback.push({ target, status: res.status, body: res.headers.get("content-type")?.includes("json") ? JSON.parse(text) : text });
}
for (const r of routes as Array<{ body: { generatedAt?: unknown } }>) {
  if (r.body && typeof r.body === "object" && "generatedAt" in r.body) r.body.generatedAt = "<now>";
}
writeFixture("routes.json", { kgSetup, flowSetup, routes, fallback });

// ─── command-router の slash 応答 ───────────────────────────────────────
freshDbs();
_resetFlowDb();
for (const s of flowSetup) getFlowDb().exec(s);
const deps = { workspaceId: "ws", adminIds: ["admin1"], discussionChannelIds: [], getEngine: () => null };
const slashCases = [
  { name: "discutere-discussions", argsText: "" },
  { name: "discutere-discussions", argsText: "draft" },
  { name: "discutere-discussions", argsText: "concluded" },
  { name: "discutere-discussions", argsText: "weird" },
  { name: "discutere-conclusions", argsText: "" },
  { name: "discutere-conclusions", argsText: "g1" },
  { name: "discutere-conclusions", argsText: "missing" },
  { name: "discutere-queue", argsText: "" },
  { name: "discutere-backup", argsText: "", userId: "admin1" },
  { name: "discutere-backup", argsText: "", userId: "intruder" },
  { name: "persona-generate", argsText: "steam", userId: "admin1" },
  { name: "economy-graph", argsText: "" },
  { name: "economy-graph", argsText: "Slay" },
  { name: "mechanics-check", argsText: "x" },
  { name: "level-check", argsText: "x" },
  { name: "discutere-status", argsText: "", userId: "admin1" },
  { name: "discutere-status", argsText: "", userId: "admin1", adminIds: [] as string[] },
  { name: "discutere-kill", argsText: "", userId: null, enabledOption: true },
  { name: "integrate", argsText: "" },
  { name: "unknowncmd", argsText: "a b" },
  { name: "propose", argsText: "新しい仮説" },
];
const slash: unknown[] = [];
for (const c of slashCases) {
  const d = { ...deps, adminIds: (c as { adminIds?: string[] }).adminIds ?? deps.adminIds };
  const reply = router.routeSlashCommand(
    {
      name: c.name,
      argsText: c.argsText,
      enabledOption: (c as { enabledOption?: boolean }).enabledOption,
      userId: (c as { userId?: string | null }).userId ?? null,
      guildId: "G1",
      channelId: "C1",
    },
    d
  );
  slash.push({ input: c, reply });
}
writeFixture("slash.json", { kgSetup, flowSetup, slash });

// ─── message-input (submitMessage) ─────────────────────────────────────
freshDbs();
{
  const core = createCore(process.env.DISCATIER_KUZU_PATH!, path.join(tmp, "events.jsonl"));
  core.client.raw.exec(`INSERT INTO sessions (id, workspace_id, title, started_at, created_at, updated_at) VALUES ('sx', 'ws', 's', 1, 1, 1)`);
  const inputs = [
    "/define parry",
    "/intends confidence",
    "/refine timing window",
    "/history parry",
    "/unknown a b",
    "/me",
    "/me role designer",
    "/cluster",
    "/hotspot",
    "/stale",
    "/scene cave",
    "/done",
    "/submit",
    "/",
    "ふつうの発言",
    "> @uid:u-prev\n返信です",
  ];
  const results = inputs.map((rawContent) => {
    const r = submitMessage({ core, workspaceId: "ws", sessionId: "sx", personId: "p1", rawContent });
    return { rawContent, hasUtterance: r.utteranceId.length > 0, commandResult: r.commandResult ?? null };
  });
  const utterances = core.client.raw
    .prepare("SELECT speaker_id, raw_content, responds_to FROM utterances WHERE session_id = 'sx' ORDER BY rowid")
    .all();
  core.close();
  writeFixture("message_input.json", { results, utterances });
}

// ─── forum-monitor の純関数 ─────────────────────────────────────────────
{
  const tags: string[][] = [[], ["面白さ"], ["改善提案"], ["改善"], ["雑談"], ["面白さ", "改善提案"], ["  IMPROVEMENT "], ["FUN"], ["", " "]];
  const scenes = ["discord:111/222", "gap:abc", "discord:111", "discord:/222", "discord:111/", "discord:a/b/c", "discord:"];
  writeFixture("forum.json", {
    directions: tags.map((t) => ({ tags: t, direction: forum.pickForumDirection(t) })),
    custom: forum.pickForumDirection(["proposal"], { improvementTagNames: ["proposal"], funTagNames: ["fun"] }),
    scenes: scenes.map((s) => ({ scene: s, parsed: forum.parseDiscordScene(s) })),
    summaries: [
      ["ヴァンサバの面白さ", "結論本文です"],
      ["題", "  "],
      ["題", "  前後の空白  \n"],
    ].map(([t, s]) => ({ title: t, summary: s, out: forum.formatForumSummary(t, s) })),
  });
}

fs.rmSync(tmp, { recursive: true, force: true });
console.log("w4-Q golden: done");
