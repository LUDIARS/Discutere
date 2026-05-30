/**
 * Discord application command の登録 (REST PUT).
 *
 * - guildIds 指定あり: 各 guild に **guild command** として登録 (反映が即時)。
 * - guildIds 空: **global command** として登録 (反映に最大 1 時間)。
 *
 * 起動時 (gateway ClientReady) と手動 script の双方から呼べる純関数。
 * token は引数で受け取り、ログには出さない。
 */

import { REST, Routes } from "discord.js";
import { DISCORD_COMMAND_DEFS } from "./command-defs.js";

export interface RegisterCommandsArgs {
  botToken: string;
  /** application id。bot の場合 client.application.id と一致する */
  applicationId: string;
  /** 登録先 guild 群。空なら global 登録 */
  guildIds?: string[];
}

export interface RegisterCommandsResult {
  scope: "guild" | "global";
  guildIds: string[];
  commandCount: number;
}

export async function registerSlashCommands(
  args: RegisterCommandsArgs
): Promise<RegisterCommandsResult> {
  if (!args.botToken) throw new Error("registerSlashCommands: botToken required");
  if (!args.applicationId) throw new Error("registerSlashCommands: applicationId required");

  const rest = new REST({ version: "10" }).setToken(args.botToken);
  const body = DISCORD_COMMAND_DEFS;
  const guildIds = (args.guildIds ?? []).filter((g) => g && g !== "dm");

  if (guildIds.length > 0) {
    for (const guildId of guildIds) {
      await rest.put(Routes.applicationGuildCommands(args.applicationId, guildId), { body });
    }
    return { scope: "guild", guildIds, commandCount: body.length };
  }

  await rest.put(Routes.applicationCommands(args.applicationId), { body });
  return { scope: "global", guildIds: [], commandCount: body.length };
}
