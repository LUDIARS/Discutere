/**
 * Discord application command を手動で登録する (npm run discord:register).
 *
 * 起動時 (gateway ClientReady) にも自動登録するが、bot を起動せずに登録だけ
 * したい / global ⇄ guild を切り替えたい場合に使う。
 *
 * 必要 config: discord.botToken, discord.applicationId (+ 任意で discord.guildIds)。
 */

import { getConfig } from "../src/config.js";
import { registerSlashCommands } from "../src/discord-hook/register-commands.js";

const config = getConfig();

if (!config.discord.botToken) {
  console.error("✗ discord.botToken が未設定です");
  process.exit(1);
}
if (!config.discord.applicationId) {
  console.error("✗ discord.applicationId が未設定です (手動登録には必須)");
  process.exit(1);
}

const result = await registerSlashCommands({
  botToken: config.discord.botToken,
  applicationId: config.discord.applicationId,
  guildIds: config.discord.guildIds,
});

console.log(
  `✅ ${result.commandCount} commands registered (${result.scope}` +
    (result.scope === "guild" ? ` → guilds: ${result.guildIds.join(", ")}` : " — 反映に最大1時間") +
    ")"
);
