// w3-M (slash コマンド定義・状態カード embed・headless seed の開幕文・JS の数値変換) の Node 実装の出力を記録する。
// 実行: npx tsx native/tests/golden/w3-M-golden.gen.ts
// 出力: native/tests/fixtures/w3m/* (値はすべて架空)
import fs from "node:fs";
import path from "node:path";
import { EmbedBuilder } from "discord.js";

import { DISCORD_COMMAND_DEFS } from "../../../src/discord-hook/command-defs.js";
import { checkLevelConsistency } from "../../../src/level-check/tool.js";
import { checkMechanicsConsistency } from "../../../src/mechanics-check/tool.js";
import { levelCheckReply } from "../../../src/discord-hook/level-check.js";
import { mechanicsCheckReply } from "../../../src/discord-hook/mechanics-check.js";
import { MockLLMClient } from "../../../src/persona-engine/llm/mock.js";
import { requestFixture as levelRequest, responseFixture as levelResponse } from "../../../tests/level-check/fixtures.js";
import { requestFixture as mechRequest, responseFixture as mechResponse } from "../../../tests/mechanics-check/fixtures.js";

const root = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "w3m");
fs.mkdirSync(root, { recursive: true });

// 1) slash コマンド定義: JSON.stringify の出力そのもの (1 バイトも違わないことを C++ で確かめる)。
fs.writeFileSync(path.join(root, "command_defs.json"), JSON.stringify(DISCORD_COMMAND_DEFS));

// 2) monitor-card の embed (MonitorCard.buildEmbed と同じ組み立て。時刻は固定)。
function buildEmbed(activeSessions: number, pendingHypotheses: number, at: number) {
  const active = activeSessions > 0;
  return new EmbedBuilder()
    .setTitle("🟢 Discutere Monitor")
    .setColor(active ? 0x57f287 : 0x99aab5)
    .addFields(
      { name: "状態", value: active ? "アクティブ" : "アイドル", inline: true },
      { name: "進行中セッション数", value: String(activeSessions), inline: true },
      { name: "未処理仮説数", value: String(pendingHypotheses), inline: true },
    )
    .setFooter({ text: "最終更新" })
    .setTimestamp(new Date(at));
}
const embeds = [
  [0, 0, 1_700_000_000_000],
  [3, 12, 1_762_000_000_123],
].map(([a, p, at]) => ({ active: a, pending: p, at, json: JSON.stringify({ embeds: [buildEmbed(a, p, at).toJSON()] }) }));
fs.writeFileSync(path.join(root, "monitor_embeds.json"), JSON.stringify(embeds, null, 2) + "\n");

// 3) Number(x) / String(x) (consensus scorer の i / score、facilitator の String(gen.name)) の JS の規則。
const values: unknown[] = [3, 2.5, -1, "4", " 5 ", "", "x", "0x10", "1e1", null, true, false, [], [7], [1, 2], {}, ["a", null, 3]];
const conv = values.map((v) => ({ v: v === undefined ? "__undefined__" : v, num: Number(v), str: String(v), isInt: Number.isInteger(Number(v)) }));
fs.writeFileSync(
  path.join(root, "js_values.json"),
  JSON.stringify(conv.map((c) => ({ ...c, num: Number.isNaN(c.num) ? "NaN" : c.num })), null, 2) + "\n"
);
// 4) level-check / mechanics-check の単発ツール (偽 LLM) と返信の形。
const reply = (r: { content?: string; files?: unknown[] }) => ({
  content: r.content ?? null,
  files: (r.files ?? []).map((f) => { const x = f as { name: string; attachment: Buffer }; return { name: x.name, data: x.attachment.toString("utf8") }; }),
});
const tools: unknown[] = [];
{
  const req = levelRequest();
  const result = await checkLevelConsistency(req, { llm: new MockLLMClient([levelResponse]) });
  tools.push({ name: "level", input: req, response: levelResponse(), result: JSON.stringify(result), reply: reply(levelCheckReply(result)) });
  const noModel = { levelText: req.levelText, baselineText: req.baselineText };
  const r2 = await checkLevelConsistency(noModel, { llm: new MockLLMClient([levelResponse]) });
  tools.push({ name: "level-no-model", input: noModel, response: levelResponse(), result: JSON.stringify(r2), reply: reply(levelCheckReply(r2)) });
}
{
  const req = mechRequest();
  const result = await checkMechanicsConsistency(req, { llm: new MockLLMClient([mechResponse]) });
  tools.push({ name: "mechanics", input: req, response: mechResponse(), result: JSON.stringify(result), reply: reply(mechanicsCheckReply(result)) });
  const obs = { ...req, references: [...(req.references ?? []), { id: "obs", source: "playtest", text: "観測した。", evidenceKind: "observation" as const }] };
  const r2 = await checkMechanicsConsistency(obs, { llm: new MockLLMClient([mechResponse]) });
  tools.push({ name: "mechanics-observed", input: obs, response: mechResponse(), result: JSON.stringify(r2), reply: reply(mechanicsCheckReply(r2)) });
  const short = mechanicsCheckReply({ logic_differences: [], design_gap: [] });
  tools.push({ name: "short-reply", reply: reply(short) });
}
fs.writeFileSync(path.join(root, "diagnostic_tools.json"), JSON.stringify(tools, null, 2) + "\n");

console.log("wrote native/tests/fixtures/w3m/{command_defs,monitor_embeds,js_values}.json");
