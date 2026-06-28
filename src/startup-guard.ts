/**
 * production 起動時の health guard。
 *
 * Discord-only pivot 後、運用前提 (botToken 必須・admin allowlist 設定) が
 * コード上の不変条件として表現されていなかった (REVIEW_DESIGN「暗黙の運用前提」/
 * REVIEW_QUALITY SRE C)。NODE_ENV=production のとき起動時に最低限の健全性を
 * fail-fast で検証し、運用ミス (token 空のまま本番起動等) を黙って無効化しない
 * (作法: 無言フォールバック禁止)。dev/test では no-op。
 */

import type { DiscutereConfig } from "./config.js";

export class StartupHealthError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "StartupHealthError";
  }
}

/**
 * production の起動前提を検証する。致命は throw、推奨外は warn (起動は続行)。
 *
 * @throws {StartupHealthError} production で必須前提 (botToken) を欠くとき
 */
export function assertStartupHealth(
  config: DiscutereConfig,
  logger: Pick<Console, "warn"> = console
): void {
  if (config.nodeEnv !== "production") return;

  // ── 致命: 欠けると議論 bot が機能しない ──
  const fatal: string[] = [];
  if (!config.discord.botToken) {
    fatal.push(
      "discord.botToken (DISCUTERE_DISCORD_BOT_TOKEN) が未設定。" +
        "production では Discord Gateway を起動できず議論 bot が機能しない"
    );
  }
  if (fatal.length > 0) {
    throw new StartupHealthError(
      "production 起動の前提条件を満たしていません:\n  - " + fatal.join("\n  - ")
    );
  }

  // ── 致命ではないが運用上の注意 (起動は続行) ──
  if (config.discord.adminIds.length === 0) {
    logger.warn(
      "[startup-guard] discord.adminIds が空です。admin slash コマンドは全 deny になります " +
        "(DISCUTERE_DISCORD_ADMIN_IDS)"
    );
  }
  if (config.llm.backend === "mock") {
    logger.warn("[startup-guard] production で llm.backend=mock です。実 LLM 応答は返りません");
  }
}
