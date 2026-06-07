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
import {
  listConclusions,
  getConclusionDetail,
  type ConclusionSummary,
  type ConclusionDetail,
} from "../visualize/conclusions.js";
import type { DiscordAutoDiscussionInput, ForumDirection } from "./auto-discussion.js";
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
  /** /discutere-queue 用の議論キューサマリ生成 (注入。未設定なら非対応応答) */
  buildQueueText?: () => string;
  /** /discutere-backup 用の手動バックアップトリガ (注入。未設定なら非対応応答) */
  triggerBackup?: () => Promise<{ ok: boolean; key?: string; bucket?: string; error?: string }>;
  /**
   * 平文投稿から議題を自動検出して persona-engine の議論開始イベントにつなぐ。
   * `started=true` は「議論の種(開始エントリ)が新規に立った」ことを表す (caller のリアクション判定用)。
   */
  classifyInboundMessage?: (
    input: DiscordAutoDiscussionInput
  ) => Promise<{ started: boolean }> | Promise<void> | void;
}

/** routeInboundMessage の結果。ingested=取り込み有無 / seed=議論の種が立ったかの非同期判定。 */
export interface InboundRouteResult {
  /** 議論チャンネルの平文として utterance に取り込んだか */
  ingested: boolean;
  /**
   * 取り込んだ投稿が「議論の種(開始エントリ)」になったかを解決する Promise。
   * 分類器(classifyInboundMessage)未設定 or 未取込なら undefined。
   */
  seed?: Promise<boolean>;
}

export function routeSlashCommand(cmd: InboundSlashCommand, deps: CommandRouterDeps): SlashReply {
  if (cmd.name === "discutere-kill" || cmd.name === "discutere-status") {
    return handleEngineSlash(cmd, deps);
  }
  if (cmd.name === "discutere-queue") {
    return handleQueueSlash(deps);
  }
  if (cmd.name === "discutere-backup") {
    return handleBackupSlash(cmd, deps);
  }
  if (cmd.name === "discutere-conclusions") {
    return handleConclusionsSlash(cmd, deps);
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
 *
 * 「自然な取り込み」: 許可チャンネルでは slash を打たずとも全平文を議論に乗せる。
 * スレッド内発言は親チャンネルが許可リストにあれば取り込む (parentChannelId で判定)。
 * session は実際の発言先 (スレッドなら thread id) に bind するので、AI 返信も同じ場所に返る。
 *
 * @returns ingested と、議論の種(開始エントリ)になったかを解決する seed Promise。
 *   caller は seed が true のときだけリアクションを付ける (取り込み全件には付けない)。
 */
export function routeInboundMessage(
  msg: DiscordInboundMessage,
  guildId: string,
  deps: CommandRouterDeps,
  parentChannelId?: string
): InboundRouteResult {
  // 議論チャンネル (または親が議論チャンネルのスレッド) のみ取り込む。
  const allowed =
    deps.discussionChannelIds.includes(msg.channelId) ||
    (!!parentChannelId && deps.discussionChannelIds.includes(parentChannelId));
  if (!allowed) return { ingested: false };
  const text = msg.content.trim();
  if (text.length === 0) return { ingested: false };
  // slash 由来 (先頭 "/") は interaction 経路で処理されるので二重取り込みしない。
  if (text.startsWith("/")) return { ingested: false };

  const core = createCore();
  try {
    const sessionId = ensureDiscordSession(core, deps.workspaceId, guildId, msg.channelId);
    const res = submitMessage({
      core,
      workspaceId: deps.workspaceId,
      sessionId,
      personId: msg.author.id,
      rawContent: text,
    });
    let seed: Promise<boolean> | undefined;
    if (res.utteranceId && deps.classifyInboundMessage) {
      seed = Promise.resolve(
        deps.classifyInboundMessage({
          workspaceId: deps.workspaceId,
          guildId,
          channelId: msg.channelId,
          sessionId,
          utteranceId: res.utteranceId,
          authorId: msg.author.id,
          content: text,
        })
      )
        .then((r) => (r != null && typeof r === "object" ? r.started === true : false))
        .catch((err) => {
          console.warn(`  discord-auto-discussion: failed: ${(err as Error).message}`);
          return false;
        });
    }
    return { ingested: true, seed };
  } finally {
    core.close();
  }
}

/**
 * フォーラムポストの取り込み (フォーラム集約)。
 *
 * フォーラム = 議論カテゴリ。ポスト (スレッド) の **最初の投稿 (starter)** が議論の種で、
 * 後続投稿は進行中議論への参加者発言になる。`discussionChannelIds` の許可ゲートに依らず
 * guild 内の全フォーラムを対象にする (channelId = thread.id なので session/AI 返信は
 * そのスレッドに紐付く)。
 *
 * - isStarter=true: utterance を取り込み、まだ議論が立っていなければ auto-discussion で
 *   designGap を起こす (seed Promise を返す)。既に当該スレッドで open な議論があれば
 *   二重起動を避けて classify しない。
 * - isStarter=false: utterance を取り込むだけ (新規 gap は立てない)。event-bridge が
 *   人間発言として進行中議論にミラーする。
 */
export function routeForumPost(
  msg: DiscordInboundMessage,
  guildId: string,
  deps: CommandRouterDeps,
  opts: { isStarter: boolean; direction?: ForumDirection }
): InboundRouteResult {
  const text = msg.content.trim();
  if (text.length === 0) return { ingested: false };
  // slash 由来 (先頭 "/") は interaction 経路で処理されるので二重取り込みしない。
  if (text.startsWith("/")) return { ingested: false };

  const core = createCore();
  try {
    const sessionId = ensureDiscordSession(core, deps.workspaceId, guildId, msg.channelId);
    const res = submitMessage({
      core,
      workspaceId: deps.workspaceId,
      sessionId,
      personId: msg.author.id,
      rawContent: text,
    });

    // starter のみ議論を起こす。既に当該スレッドで open な議論があれば二重起動しない。
    const shouldSeed =
      opts.isStarter &&
      !!res.utteranceId &&
      !!deps.classifyInboundMessage &&
      !forumDiscussionExists(core, deps.workspaceId, guildId, msg.channelId);
    if (!shouldSeed) return { ingested: true };

    const seed = Promise.resolve(
      deps.classifyInboundMessage!({
        workspaceId: deps.workspaceId,
        guildId,
        channelId: msg.channelId,
        sessionId,
        utteranceId: res.utteranceId!,
        authorId: msg.author.id,
        content: text,
        direction: opts.direction,
      })
    )
      .then((r) => (r != null && typeof r === "object" ? r.started === true : false))
      .catch((err) => {
        console.warn(`  discord-forum: classify failed: ${(err as Error).message}`);
        return false;
      });
    return { ingested: true, seed };
  } finally {
    core.close();
  }
}

/** 当該スレッド (scene=discord:<guild>/<threadId>) で open な議論 session が既にあるか。 */
function forumDiscussionExists(
  core: ReturnType<typeof createCore>,
  workspaceId: string,
  guildId: string,
  threadId: string
): boolean {
  const scene = `discord:${guildId}/${threadId}`;
  const row = core.client.raw
    .prepare(
      `SELECT s.id AS id
         FROM sessions s
         JOIN design_gaps g
           ON g.id = SUBSTR(s.title, LENGTH('discussion-of-gap:') + 1)
        WHERE s.workspace_id = ?
          AND s.title LIKE 'discussion-of-gap:%'
          AND s.scene = ?
          AND (g.status IS NULL OR g.status NOT IN ('closed', 'converged', 'dismissed'))
        LIMIT 1`
    )
    .get(workspaceId, scene) as { id: string } | undefined;
  return !!row;
}

/** /discutere-queue — 議論キューのサマリを ephemeral で返す (全員可) */
function handleQueueSlash(deps: CommandRouterDeps): SlashReply {
  if (!deps.buildQueueText) {
    return { content: "⚠️ queue view が利用できません (起動構成を確認)", ephemeral: true };
  }
  try {
    return { content: deps.buildQueueText(), ephemeral: true };
  } catch (err) {
    return { content: `⚠️ queue 取得失敗: ${(err as Error).message}`, ephemeral: true };
  }
}

/**
 * /discutere-backup — 手動 S3 バックアップ (admin only)。
 * tar+upload は数秒かかり得るので fire-and-forget で起動し、即時 ack を返す
 * (Discord interaction の 3 秒 ack 制限回避)。結果はサーバログ / dashboard で確認。
 */
function handleBackupSlash(cmd: InboundSlashCommand, deps: CommandRouterDeps): SlashReply {
  if (deps.adminIds.length === 0) {
    return { content: "⚠️ discord.adminIds 未設定 — admin コマンドは無効", ephemeral: true };
  }
  if (!cmd.userId || !deps.adminIds.includes(cmd.userId)) {
    return { content: "⚠️ admin only", ephemeral: true };
  }
  if (!deps.triggerBackup) {
    return { content: "⚠️ backup が構成されていません (backup.bucket 未設定?)", ephemeral: true };
  }
  void deps
    .triggerBackup()
    .then((r) => {
      if (r.ok) console.log(`  backup(manual): uploaded s3://${r.bucket}/${r.key}`);
      else console.warn(`  backup(manual): failed: ${r.error}`);
    })
    .catch((e) => console.warn(`  backup(manual): error: ${(e as Error).message}`));
  return { content: "🗄️ バックアップを開始しました (完了はサーバログ / dashboard で確認)", ephemeral: true };
}

/**
 * /discutere-conclusions — 収束した議論の結論を一覧 / gap 指定で論述データを表示 (#66)。
 * 認可不要 (読み取り専用)。 ephemeral で返す。
 */
function handleConclusionsSlash(cmd: InboundSlashCommand, deps: CommandRouterDeps): SlashReply {
  const core = createCore();
  try {
    const gapId = cmd.argsText.trim();
    if (gapId) {
      const detail = getConclusionDetail(core, deps.workspaceId, gapId);
      if (!detail) return { content: `⚠️ 結論が見つかりません (gap=${gapId})`, ephemeral: true };
      return { content: formatConclusionDetail(detail), ephemeral: true };
    }
    const list = listConclusions(core, deps.workspaceId, 10);
    return { content: formatConclusionList(list), ephemeral: true };
  } catch (err) {
    return { content: `⚠️ 結論取得失敗: ${(err as Error).message}`, ephemeral: true };
  } finally {
    core.close();
  }
}

function formatConclusionList(list: ConclusionSummary[]): string {
  if (list.length === 0) return "まだ収束した議論はありません。";
  const lines = list.map((c) => {
    const head = c.conclusion ? truncateText(c.conclusion, 120) : "(まとめ未生成)";
    return `• **${c.title}**\n  ${head}\n  └ 詳細: /discutere-conclusions gap:${c.gapId} (発言${c.utteranceCount}/止揚${c.aufhebungCount})`;
  });
  return [`【結論一覧 (新しい順 ${list.length}件)】`, ...lines].join("\n");
}

function formatConclusionDetail(d: ConclusionDetail): string {
  const parts: string[] = [`【結論】${d.title}`];
  parts.push(d.conclusion ? d.conclusion : "(まとめ未生成)");
  if (d.aufhebungen.length) {
    parts.push("\n── 止揚ストック ──");
    parts.push(d.aufhebungen.map((s, i) => `${i + 1}. ${s}`).join("\n"));
  }
  if (d.topOpinions.length) {
    parts.push("\n── 高評価意見 ──");
    parts.push(d.topOpinions.map((o) => `+${o.score} ${o.speaker}: ${truncateText(o.content, 100)}`).join("\n"));
  }
  parts.push(`\n── 議論ログ (${d.transcript.length}発言) ──`);
  parts.push(
    d.transcript.map((u) => `[${u.speaker}] ${truncateText(u.content, 120)}`).join("\n")
  );
  return truncateText(parts.join("\n"), 1900); // Discord 2000 字制限に収める
}

function truncateText(value: string, max: number): string {
  const flat = value.replace(/\n+/g, (m) => (m.length > 1 ? "\n" : m));
  return flat.length <= max ? flat : `${flat.slice(0, max - 1)}…`;
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
