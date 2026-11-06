// w5-U 担当 (flow/discord-live.ts) の Node 実装がスレッドへ投稿する本文を記録する。
// 実行: npx tsx native/tests/golden/w5-U-discord_live.gen.ts → native/tests/fixtures/w5u/discord_live.json
//
// Discord REST は globalThis.fetch を差し替えて受ける (discord.com へは出ない)。LLM は常に失敗を返す偽物
// (失敗時の決定的な縮退経路を通す)。ユーザーの声の外部収集を避けるため観点タグは「機密」。
// 記録するのは bot 名義の投稿 (POST /channels/{id}/messages の content) の列。C++ は同じ入力で同じ列を作る
// (承認後の議論本体は w4-P の golden が確かめるので、承認の通知までを比べる)。
// 時刻・UUID は投稿本文に出ない。data/ は読まない (gamesDir は空の一時ディレクトリ)。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "w5u-golden-"));
process.env.DISCUTERE_CONFIG = path.join(tmp, "no-config.json");
process.env.DATABASE_PATH = path.join(tmp, "flow.db");
process.env.DISCUTERE_FLOW_PAPER_REVIEW_ENABLED = "true";
delete process.env.DISCUTERE_FLOW_PAPER_REVIEW_TIMEOUT_MS;
delete process.env.DISCUTERE_YOUTUBE_API_KEY;
const gamesDir = path.join(tmp, "games");
fs.mkdirSync(gamesDir, { recursive: true });

const here = path.dirname(new URL(import.meta.url).pathname);
const fixturesDir = path.join(here, "..", "fixtures", "w5u");
fs.mkdirSync(fixturesDir, { recursive: true });

type Post = { channel: string; content: string };
const posts: Post[] = [];
let seq = 0;
const realFetch = globalThis.fetch;
globalThis.fetch = (async (input: string | URL | Request, init?: RequestInit) => {
  const url = typeof input === "string" ? input : input instanceof URL ? input.href : input.url;
  if (!url.startsWith("https://discord.com/api/v10/")) return realFetch(input as string, init);
  const p = url.slice("https://discord.com/api/v10".length);
  const m = p.match(/^\/channels\/([^/]+)\/messages$/);
  if (m && (init?.method ?? "GET") === "POST") {
    const body = JSON.parse(String(init?.body ?? "{}"));
    posts.push({ channel: decodeURIComponent(m[1]), content: body.content });
    return new Response(JSON.stringify({ id: `m${++seq}` }), { status: 200, headers: { "content-type": "application/json" } });
  }
  return new Response(JSON.stringify({ message: "not found" }), { status: 404 });
}) as typeof fetch;

const live = await import("../../../src/flow/discord-live.js");

const llm = { invoke: async () => ({ ok: false as const, error: "golden" }) };
const voiceRequest = {
  submit: async (requestId: string) => ({ accepted: true, requestId }),
  status: async (requestId: string) => ({ accepted: true, requestId }),
};
const deps = { botToken: "x", llm, gamesDir, voiceRequest } as unknown as Parameters<typeof live.startForumFlow>[1];

const scenarios: Record<string, unknown> = {};

// A. 学習: Core 未設定なら起動できない旨を出す。
posts.length = 0;
await live.startForumFlow({ guildId: "G1", threadId: "TL", theme: "学習テーマ", flow: "learning", tags: [] }, deps);
scenarios.learningNoCore = posts.slice();

// B. 議論 + レビューゲート: 準備 → 草案 → 声の準備 → 案内 → 返信 (戻す / 相づち / 調整 / 受付確認) → 開始。
posts.length = 0;
live._resetFlowLive();
await live.startForumFlow({ guildId: "G1", threadId: "TR", theme: "周回の面白さ", flow: "discussion", tags: ["機密"] }, deps);
const marks: number[] = [posts.length];
for (const text of ["戻す", "はい", "メカニクスにガチャを追加", "受付確認"]) {
  await live.handlePaperReviewReply("TR", "G1", text, deps);
  marks.push(posts.length);
}
const hadReview = live.hasPaperReview("TR");
await live.handlePaperReviewReply("TR", "G1", "開始", deps);
const approvedAt = posts.findIndex((p) => p.content.startsWith("✅ ペーパーを承認しました"));
scenarios.review = {
  posts: posts.slice(0, approvedAt + 1),
  marks,
  hadReview,
};

// C. 承認案内 (タイムアウトの表記)。
const { loadConfig } = await import("../../../src/config.js");
void loadConfig;
scenarios.approvalGuideDefault = posts.find((p) => p.content.startsWith("✏️"))?.content ?? null;

fs.writeFileSync(path.join(fixturesDir, "discord_live.json"), JSON.stringify(scenarios, null, 2) + "\n");
console.log("wrote", path.join(fixturesDir, "discord_live.json"));
process.exit(0);
