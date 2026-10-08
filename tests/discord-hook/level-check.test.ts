import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { ApplicationCommandOptionType, MessageFlags, type ChatInputCommandInteraction, type InteractionEditReplyOptions } from "discord.js";
import { DISCORD_COMMAND_DEFS } from "../../src/discord-hook/command-defs.js";
import { handleLevelCheckCommand, levelCheckReply, readLevelCheckSlashInput } from "../../src/discord-hook/level-check.js";
import { routeSlashCommand } from "../../src/discord-hook/command-router.js";
import { LEVEL_CHECK_LIMITS as L } from "../../src/level-check/contracts.js";
import { MockLLMClient } from "../../src/persona-engine/llm/mock.js";
import { differenceFixture, requestFixture, responseFixture } from "../level-check/fixtures.js";

const command = DISCORD_COMMAND_DEFS.find((c) => c.name === "level-check");
assert.ok(command);
assert.equal(command.options?.[0].name, "level");
assert.equal(command.options?.[0].type, ApplicationCommandOptionType.String);
assert.equal(command.options?.[0].required, true);
assert.equal(command.options?.[2].name, "input");
const request = requestFixture();
const input = JSON.stringify({ references: request.references, playModel: request.playModel });
assert.deepEqual(readLevelCheckSlashInput(request.levelText, request.baselineText!, input).playModel, request.playModel);
assert.throws(() => readLevelCheckSlashInput("spec", null, "[]"));
assert.throws(() => readLevelCheckSlashInput("spec", null, "{}junk"));
assert.throws(() => readLevelCheckSlashInput("spec", null, JSON.stringify({ levelText: "override" })));
assert.throws(() => readLevelCheckSlashInput("spec", null, "x".repeat(L.discordInputChars + 1)));

let replies: InteractionEditReplyOptions[] = [];
let lifecycle: string[] = [];
function interaction(name = "level-check", level = request.levelText): Pick<ChatInputCommandInteraction, "commandName" | "options" | "deferReply" | "editReply"> {
  return {
    commandName: name,
    options: { getString: (key: string) => { const values: Record<string, string | undefined> = { level, baseline: request.baselineText, input }; return values[key] ?? null; } } as ChatInputCommandInteraction["options"],
    deferReply: (async (options: { flags?: number }) => { lifecycle.push("defer"); assert.equal(options.flags, MessageFlags.Ephemeral); }) as unknown as ChatInputCommandInteraction["deferReply"],
    editReply: (async (reply: InteractionEditReplyOptions) => { lifecycle.push("edit"); replies.push(reply); }) as unknown as ChatInputCommandInteraction["editReply"],
  };
}
const llm = new MockLLMClient([() => { lifecycle.push("llm"); return responseFixture(); }]);
assert.equal(await handleLevelCheckCommand(interaction(), { llm }), true);
assert.deepEqual(lifecycle, ["defer", "llm", "edit"]);
assert.equal(replies.length, 1);
assert.equal(llm.calls, 1);
assert.deepEqual(replies[0].allowedMentions, { parse: [] });
assert.equal(replies[0].files?.length, 1);
const attached = replies[0].files?.[0] as { name: string; attachment: Buffer };
assert.equal(attached.name, "level-check.json");
assert.ok(Buffer.isBuffer(attached.attachment));
assert.deepEqual(Object.keys(JSON.parse(attached.attachment.toString("utf8"))), ["logic_differences", "design_gap"]);

for (const deps of [undefined, { llm: new MockLLMClient([() => ({ ok: false, error: "secret-token" })]) }, { llm: new MockLLMClient([() => "bad JSON"]) }]) {
  replies = []; lifecycle = [];
  await handleLevelCheckCommand(interaction(), deps);
  assert.deepEqual(lifecycle, ["defer", "edit"]);
  assert.equal(replies.length, 1);
  assert.match(replies[0].content ?? "", /診断失敗/);
  assert.doesNotMatch(replies[0].content ?? "", /secret-token/);
  assert.deepEqual(replies[0].allowedMentions, { parse: [] });
}
replies = []; lifecycle = [];
assert.equal(await handleLevelCheckCommand(interaction("another-command")), false);
assert.equal(replies.length, 0);
assert.equal(lifecycle.length, 0);
const short = levelCheckReply({ logic_differences: [], design_gap: [] });
assert.match(short.content ?? "", /logic_differences/);
assert.deepEqual(short.allowedMentions, { parse: [] });
assert.throws(() => levelCheckReply({ logic_differences: [{ ...differenceFixture(), target: "x".repeat(L.resultBytes) }], design_gap: [] }), /byte limit/);

// This guard returns before any DB/session/discussion dependency is inspected.
const guarded = routeSlashCommand({ name: "level-check", argsText: "spec", userId: "u", guildId: "g", channelId: "c" }, {
  workspaceId: "test", adminIds: [], discussionChannelIds: [], getEngine: () => { throw new Error("must not access discussion engine"); },
});
assert.match(guarded.content, /dedicated async handler/);
const deliveryFailure = interaction();
deliveryFailure.editReply = (async () => { throw new Error("transport failed"); }) as ChatInputCommandInteraction["editReply"];
await assert.rejects(handleLevelCheckCommand(deliveryFailure, { llm: new MockLLMClient([responseFixture]) }), /transport failed/);
const deferFailure = interaction();
deferFailure.deferReply = (async () => { throw new Error("transport failed"); }) as ChatInputCommandInteraction["deferReply"];
const notInvoked = new MockLLMClient([responseFixture]);
await assert.rejects(handleLevelCheckCommand(deferFailure, { llm: notInvoked }), /transport failed/);
assert.equal(notInvoked.calls, 0);
const gateway = readFileSync(new URL("../../src/discord-hook/gateway.ts", import.meta.url), "utf8");
const handlerPosition = gateway.indexOf("if (interaction.commandName === MECHANICS_CHECK_COMMAND_NAME || interaction.commandName === LEVEL_CHECK_COMMAND_NAME)");
assert.ok(handlerPosition > 0 && handlerPosition < gateway.indexOf("const cmd = toInboundSlashCommand(interaction);"));
assert.match(gateway.slice(handlerPosition, gateway.indexOf("const cmd = toInboundSlashCommand(interaction);")), /catch\s*\{[\s\S]*reply delivery failed[\s\S]*return;/);
console.log("level-check Discord adapter: all passed");
