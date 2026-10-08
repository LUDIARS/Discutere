/** Dedicated one-reply adapter; never enters the generic discussion router. */
import { MessageFlags, type ChatInputCommandInteraction, type InteractionEditReplyOptions } from "discord.js";
import { MECHANICS_CHECK_LIMITS as L, MechanicsCheckError, type MechanicsCheckRequest, type MechanicsCheckResult } from "../mechanics-check/contracts.js";
import { validateMechanicsCheckRequest } from "../mechanics-check/request.js";
import { checkMechanicsConsistency, type MechanicsCheckDependencies } from "../mechanics-check/tool.js";
import { object } from "../design-diagnostic/validation.js";

import { diagnosticReply, diagnosticFailure } from "./diagnostic-reply.js";

export const MECHANICS_CHECK_COMMAND_NAME = "mechanics-check";
type MechanicsCheckInteraction = Pick<ChatInputCommandInteraction, "commandName" | "options" | "deferReply" | "editReply">;

/** Numeric-model input is parsed but explicitly rejected by the mechanics boundary. */
export function readMechanicsCheckSlashInput(specText: string, baselineText: string | null, inputText: string | null): MechanicsCheckRequest {
  let extra: Record<string, unknown> = {};
  if (inputText !== null) {
    if (inputText.length > L.discordInputChars) throw new MechanicsCheckError("invalid_input", "input exceeds Discord JSON limit");
    let raw: unknown;
    try { raw = JSON.parse(inputText); } catch { throw new MechanicsCheckError("invalid_input", "input must be valid JSON"); }
    extra = object(raw, "input", ["references", "playModel"], "invalid_input");
  }
  return validateMechanicsCheckRequest({ specText, ...(baselineText === null ? {} : { baselineText }), ...extra });
}

/** JSON content is bounded in bytes and mentions are disabled for both delivery modes. */
export function mechanicsCheckReply(result: MechanicsCheckResult): InteractionEditReplyOptions {
  return diagnosticReply(result, "mechanics-check.json");
}

export async function handleMechanicsCheckCommand(interaction: MechanicsCheckInteraction, deps?: MechanicsCheckDependencies): Promise<boolean> {
  if (interaction.commandName !== MECHANICS_CHECK_COMMAND_NAME) return false;
  await interaction.deferReply({ flags: MessageFlags.Ephemeral });
  let reply: InteractionEditReplyOptions;
  try {
    if (!deps?.llm) throw new MechanicsCheckError("llm_unavailable", "LLM not configured");
    const request = readMechanicsCheckSlashInput(
      interaction.options.getString("spec", true),
      interaction.options.getString("baseline"),
      interaction.options.getString("input"),
    );
    reply = mechanicsCheckReply(await checkMechanicsConsistency(request, deps));
  } catch (error) {
    reply = { content: diagnosticFailure(error), allowedMentions: { parse: [] } };
  }
  await interaction.editReply(reply);
  return true;
}
