// discord-hook の純関数・discord.js ビルダーの出力を記録する。
// 実行: npx tsx native/tests/golden/w1-D-discord_hook.gen.ts
// 出力: native/tests/fixtures/discord_hook/*.json
import fs from "node:fs";
import path from "node:path";
import {
  buildFlowPickMenu,
  buildFlowSettingsComponents,
  buildFlowSettingsModal,
  mergeForumTags,
  parseSettingsPreset,
} from "../../../src/discord-hook/forum-flow-tags.js";
import { buildPaperGapModal, formatPaperGapReviewInstruction, PAPER_GAP_TYPES } from "../../../src/discord-hook/paper-gap-ui.js";
import { humanizeForDiscord } from "../../../src/discord-hook/poster.js";
import {
  formatDirectivesBlock,
  normalizeDirectiveText,
  stripBotMention,
} from "../../../src/discord-hook/facilitator-directives.js";
import { normalizeDiscordInboundMessage } from "../../../src/discord-hook/normalize.js";
import { diagnosticFailure, diagnosticReply } from "../../../src/discord-hook/diagnostic-reply.js";
import { DiagnosticError } from "../../../src/design-diagnostic/contracts.js";

const dir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "discord_hook");
fs.mkdirSync(dir, { recursive: true });
const write = (name: string, value: unknown) => fs.writeFileSync(path.join(dir, name), JSON.stringify(value, null, 2) + "\n");

write("builders.json", {
  flowPickMenu: buildFlowPickMenu("999").toJSON(),
  flowSettings: buildFlowSettingsComponents("t1").map((r) => r.toJSON()),
  flowSettingsModal: buildFlowSettingsModal("t2").toJSON(),
  paperGapModals: PAPER_GAP_TYPES.map((t) => buildPaperGapModal({ type: t.value, channelId: "ch9" }).toJSON()),
});

const presets = ["default", "3x6", "10x20", "x6", "3x", "03x6", " 3x6", "3x6 ", "3X6", "99999999999999999999x1"];
const tagCases = [
  [],
  [{ name: "議論" }, { name: "雑談" }],
  [{ id: "p", name: "議論", moderated: true, emoji: { id: null, name: "🔥" } }, { id: "s", name: "壁打ち" }, { name: "学習" }, { name: " Learning " }],
  Array.from({ length: 19 }, (_, i) => ({ name: `t${i}` })),
  Array.from({ length: 25 }, (_, i) => ({ name: `t${i}` })),
];
write("pure.json", {
  presets: presets.map((p) => ({ input: p, output: parseSettingsPreset(p) })),
  mergeForumTags: tagCases.map((c) => ({ input: c, output: mergeForumTags(c as never) })),
  humanize: [
    "反証: それは成り立たない。",
    "反例：〇〇のケース。",
    "  懸念 ： 大丈夫?本当に!  ",
    "これは良い。でも穴がある。",
    "A。\nB。",
    "本当に?確かめた?",
    "指摘:a。b！c？d!e?",
    "反論",
    "",
  ].map((t) => ({ input: t, output: humanizeForDiscord(t) })),
  stripBotMention: [
    ["<@123> もっと簡単に", "123"],
    ["<@!123>  否定的に  ", "123"],
    ["<@1234> x <@123>", "123"],
    ["a\n\tb", null],
    ["<@123>", "123"],
  ].map(([c, b]) => ({ content: c, botId: b, output: stripBotMention(c as string, b as string | null) })),
  normalizeDirective: ["   ", "x".repeat(300), "x".repeat(301), " 日本語 ", "あ".repeat(400)].map((t) => ({
    input: t,
    output: normalizeDirectiveText(t),
  })),
  directivesBlock: [[], ["簡単に", "否定的に"]].map((d) => ({ input: d, output: formatDirectivesBlock(d) })),
  paperGapInstruction: [
    { type: "user_reaction", target: "中盤のテンポ", detail: " 詳細 ", source: "想定ユーザ反応" },
    { type: "other", detail: "x", target: "  ", source: "" },
  ].map((i) => ({ input: i, output: formatPaperGapReviewInstruction(i as never) })),
  normalize: [
    { id: "1", channel_id: "c", author: { id: "a", username: "u", bot: false }, content: " hi ", mentions: [{ id: "x" }, 3, { username: " n " }], timestamp: "t" },
    { t: "MESSAGE_CREATE", d: { id: "1", channel_id: "c", author: { id: "a", username: "u" }, content: "x", timestamp: "t" } },
    { event: { event: { id: "1", channel_id: "c", author: { id: "a", username: "u" }, content: "x", timestamp: "t", mentions: "no" } } },
    { id: "1", channel_id: "c", author: { id: "a", username: "u" }, content: "   ", timestamp: "t" },
    { id: "1", channel_id: "c", author: { id: "a" }, content: "x", timestamp: "t" },
    null,
    "str",
  ].map((p) => ({ input: p, output: normalizeDiscordInboundMessage(p) })),
});

const small = { logic_differences: [], design_gap: [] };
const big = { logic_differences: [{ kind: "revision", target: "x".repeat(2000) }], design_gap: [] };
const r1 = diagnosticReply(small as never, "level-check.json");
const r2 = diagnosticReply(big as never, "mechanics-check.json");
write("diagnostic.json", {
  small: { content: r1.content, files: r1.files ?? null },
  big: { content: r2.content, fileName: (r2.files as Array<{ name: string }>)[0].name, fileText: ((r2.files as Array<{ attachment: Buffer }>)[0].attachment).toString("utf8") },
  failures: ["invalid_input", "use_level_check", "llm_unavailable", "llm_failed", "llm_timeout", "invalid_response", "result_too_large"].map(
    (c) => ({ code: c, output: diagnosticFailure(new DiagnosticError(c as never, "m")) })
  ),
  failureOther: diagnosticFailure(new Error("x")),
});
console.log("wrote discord_hook fixtures");
