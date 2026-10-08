/** Dedicated one-reply adapter; never enters the generic discussion router. */
import { MessageFlags, type ChatInputCommandInteraction, type InteractionEditReplyOptions } from "discord.js";
import { MECHANICS_CHECK_LIMITS as L, MechanicsCheckError, type MechanicsCheckRequest, type MechanicsCheckResult } from "../mechanics-check/contracts.js";
import { validateMechanicsCheckRequest } from "../mechanics-check/request.js";
import { checkMechanicsConsistency, type MechanicsCheckDependencies } from "../mechanics-check/tool.js";
import { object } from "../mechanics-check/validation.js";

export const MECHANICS_CHECK_COMMAND_NAME = "mechanics-check";
type MechanicsCheckInteraction = Pick<ChatInputCommandInteraction, "commandName" | "options" | "deferReply" | "editReply">;

/** Inline JSON is bounded and contains only the extra structured documents/model. */
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
  const json = JSON.stringify(result, null, 2);
  const bytes = Buffer.from(json, "utf8");
  if (bytes.length > L.resultBytes) throw new MechanicsCheckError("result_too_large", "Result attachment exceeds byte limit");
  const allowedMentions = { parse: [] as [] };
  if (json.length <= 1_850) return { content: `\`\`\`json\n${json}\n\`\`\``, allowedMentions };
  return { content: "logic_differences / design_gap", files: [{ attachment: bytes, name: "mechanics-check.json" }], allowedMentions };
}

function failureContent(error: unknown): string {
  const code = error instanceof MechanicsCheckError ? error.code : "llm_failed";
  const reasons: Record<typeof code, string> = {
    invalid_input: "入力の型・長さ・ID・単位・確率を確認してください。",
    llm_unavailable: "LLM が設定されていません。",
    llm_failed: "LLM の単発診断に失敗しました。",
    llm_timeout: "LLM の単発診断が時間内に完了しませんでした。",
    invalid_response: "LLM のJSON結果または根拠参照が不正です。",
    result_too_large: "診断結果が回答サイズの上限を超えました。",
  };
  return `診断失敗 (${code}): ${reasons[code]}`;
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
    reply = { content: failureContent(error), allowedMentions: { parse: [] } };
  }
  await interaction.editReply(reply);
  return true;
}
