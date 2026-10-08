/** Dedicated one-reply adapter; never enters the generic discussion router. */
import { MessageFlags, type ChatInputCommandInteraction, type InteractionEditReplyOptions } from "discord.js";
import { LEVEL_CHECK_LIMITS as L, LevelCheckError, type LevelCheckRequest, type LevelCheckResult } from "../level-check/contracts.js";
import { validateLevelCheckRequest } from "../level-check/request.js";
import { checkLevelConsistency, type LevelCheckDependencies } from "../level-check/tool.js";
import { object } from "../design-diagnostic/validation.js";

import { diagnosticReply, diagnosticFailure } from "./diagnostic-reply.js";

export const LEVEL_CHECK_COMMAND_NAME = "level-check";
type LevelCheckInteraction = Pick<ChatInputCommandInteraction, "commandName" | "options" | "deferReply" | "editReply">;

/** Inline JSON supplies only bounded references and the optional level model. */
export function readLevelCheckSlashInput(levelText: string, baselineText: string | null, inputText: string | null): LevelCheckRequest {
  let extra: Record<string, unknown> = {};
  if (inputText !== null) {
    if (inputText.length > L.discordInputChars) throw new LevelCheckError("invalid_input", "input exceeds Discord JSON limit");
    let raw: unknown;
    try { raw = JSON.parse(inputText); } catch { throw new LevelCheckError("invalid_input", "input must be valid JSON"); }
    extra = object(raw, "input", ["references", "playModel"], "invalid_input");
  }
  return validateLevelCheckRequest({ levelText, ...(baselineText === null ? {} : { baselineText }), ...extra });
}

/** JSON content is bounded in bytes and mentions are disabled for both delivery modes. */
export function levelCheckReply(result: LevelCheckResult): InteractionEditReplyOptions {
  return diagnosticReply(result, "level-check.json");
}

export async function handleLevelCheckCommand(interaction: LevelCheckInteraction, deps?: LevelCheckDependencies): Promise<boolean> {
  if (interaction.commandName !== LEVEL_CHECK_COMMAND_NAME) return false;
  await interaction.deferReply({ flags: MessageFlags.Ephemeral });
  let reply: InteractionEditReplyOptions;
  try {
    if (!deps?.llm) throw new LevelCheckError("llm_unavailable", "LLM not configured");
    const request = readLevelCheckSlashInput(
      interaction.options.getString("level", true),
      interaction.options.getString("baseline"),
      interaction.options.getString("input"),
    );
    reply = levelCheckReply(await checkLevelConsistency(request, deps));
  } catch (error) {
    reply = { content: diagnosticFailure(error), allowedMentions: { parse: [] } };
  }
  await interaction.editReply(reply);
  return true;
}
