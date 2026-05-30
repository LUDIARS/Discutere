/**
 * Transport 非依存の Discord → Discatier ルーティング (WS Gateway 再設計).
 *
 * 旧 `src/api/discord-routes.ts` に HTTP 受け口と混在していたドメインロジックを
 * ここに切り出す。Gateway (discord.js) と (将来の) HTTP fallback の双方から
 * 同じロジックを呼べるようにするための分離 (SRP)。
 *
 * - routeSlashCommand: slash command (interaction) を処理し返信文を返す。
 *   * discutere-kill / discutere-status → persona-engine 制御 (admin allowlist)
 *   * それ以外 → Discatier の `/<name> <args>` に組み立て submitMessage へ
 * - routeInboundMessage: 平文メッセージを discord-bound session の utterance に記録。
 */

import type { PersonaEngineHandle } from "../persona-engine/index.js";
import { createCore } from "../core/index.js";
import { submitMessage } from "../core/projection/message-input.js";
import type { DiscordInboundMessage } from "./types.js";

export interface SlashReply {
  content: string;
  ephemeral: boolean;
}

export interface InboundSlashCommand {
  /** command 名 (例 "propose", "discutere-kill") */
  name: string;
  /** option 値を空白連結したもの (例 "選択肢を絞る 詳細")。無ければ空文字 */
  argsText: string;
  /** discutere-kill の enabled boolean option (あれば) */
  enabledOption?: boolean;
  /** 発話者 Discord user id (DM/guild 双方) */
  userId: string | null;
  /** guild id (DM なら "dm") */
  guildId: string;
  /** channel id (不明なら "default") */
  channelId: string;
}

export interface CommandRouterDeps {
  workspaceId: string;
  /** admin slash の認可 allowlist。空なら全 deny (安全 default) */
  adminIds: string[];
  /**
   * 平文メッセージを utterance 取り込みする議論チャンネル id の許可リスト。
   * 空なら取り込まない (= 無関係チャンネルのノイズ混入を防ぐ安全 default)。
   */
  discussionChannelIds: string[];
  /** persona-engine の取得 (起動後に注入される singleton 想定) */
  getEngine: () => PersonaEngineHandle | null;
}

export function routeSlashCommand(cmd: InboundSlashCommand, deps: CommandRouterDeps): SlashReply {
  if (cmd.name === "discutere-kill" || cmd.name === "discutere-status") {
    return handleEngineSlash(cmd, deps);
  }

  const commandText = cmd.argsText.length > 0 ? `/${cmd.name} ${cmd.argsText}` : `/${cmd.name}`;
  const core = createCore();
  try {
    const sessionId = ensureDiscordSession(core, deps.workspaceId, cmd.guildId, cmd.channelId);
    const res = submitMessage({
      core,
      workspaceId: deps.workspaceId,
      sessionId,
      rawContent: commandText,
    });
    const result = res.commandResult;
    const message = result
      ? result.ok
        ? `✅ ${result.message ?? "ok"}`
        : `⚠️ ${result.error ?? "command failed"}`
      : "message routed";
    return { content: message, ephemeral: !result?.ok };
  } finally {
    core.close();
  }
}

/**
 * 平文メッセージ (slash でない発話) を discord-bound session の utterance に記録。
 * persona-engine は events table 経由で反応するため、ここでは utterance の記録に徹する。
 * bot 自身のメッセージは caller 側で除外する前提。
 */
export function routeInboundMessage(
  msg: DiscordInboundMessage,
  guildId: string,
  deps: CommandRouterDeps
): void {
  // 議論チャンネルとして許可されたチャンネルのみ取り込む (無関係チャンネルのノイズ防止)。
  if (!deps.discussionChannelIds.includes(msg.channelId)) return;
  const text = msg.content.trim();
  if (text.length === 0) return;
  // slash 由来 (先頭 "/") は interaction 経路で処理されるので二重取り込みしない。
  if (text.startsWith("/")) return;

  const core = createCore();
  try {
    const sessionId = ensureDiscordSession(core, deps.workspaceId, guildId, msg.channelId);
    submitMessage({
      core,
      workspaceId: deps.workspaceId,
      sessionId,
      personId: msg.author.id,
      rawContent: text,
    });
  } finally {
    core.close();
  }
}

/**
 * /discutere-kill / /discutere-status の handler。
 * 認可: deps.adminIds allowlist。未設定なら全 reject (安全 default、設定必須)。
 */
function handleEngineSlash(cmd: InboundSlashCommand, deps: CommandRouterDeps): SlashReply {
  if (deps.adminIds.length === 0) {
    return {
      content: "⚠️ discord.adminIds not set — admin commands are disabled",
      ephemeral: true,
    };
  }
  if (!cmd.userId || !deps.adminIds.includes(cmd.userId)) {
    return { content: "⚠️ admin only", ephemeral: true };
  }

  const engine = deps.getEngine();
  if (!engine) {
    return { content: "⚠️ persona-engine not initialized", ephemeral: true };
  }

  if (cmd.name === "discutere-status") {
    const personas = engine.personas.list().length;
    const rules = engine.rules.list({ enabled: true }).length;
    return {
      content: `🤖 persona-engine\n personas: \`${personas}\`\n active rules: \`${rules}\``,
      ephemeral: true,
    };
  }

  // discutere-kill
  if (cmd.enabledOption === undefined) {
    return { content: "usage: /discutere-kill enabled:true|false", ephemeral: true };
  }
  engine.setRulesEnabled(cmd.enabledOption);
  return {
    content: cmd.enabledOption
      ? "✅ persona-engine **ENABLED** (rules running)"
      : "🛑 persona-engine **DISABLED** (rules paused)",
    ephemeral: true,
  };
}

function ensureDiscordSession(
  core: ReturnType<typeof createCore>,
  workspaceId: string,
  guildId: string,
  channelId: string
): string {
  const title = `discord-session:${guildId}:${channelId}`;
  const existing = core.client.raw
    .prepare(
      "SELECT id FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1"
    )
    .get(workspaceId, title) as { id: string } | undefined;
  if (existing) return existing.id;
  return core.repos.session.create({
    workspaceId,
    title,
    startedAt: Date.now(),
    scene: `discord:${guildId}/${channelId}`,
  } as never);
}
