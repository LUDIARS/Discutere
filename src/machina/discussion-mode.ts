/**
 * 繝√Ε繝ｳ繝阪Ν繝｢繝ｼ繝峨瑚ｭｰ隲悶阪・蜃ｦ逅・勣縲・
 *
 * 繝輔Ο繝ｼ:
 *   1. 繝｡繝・そ繝ｼ繧ｸ謚慕ｨｿ繧貞女菫｡縺吶ｋ縺ｨ縲√メ繝｣繝ｳ繝阪Ν蜊倅ｽ阪〒縲君 蛻・ｾ後・隕∫ｴ・ち繧､繝槭・縲阪ｒ逋ｻ骭ｲ (debounce)
 *   2. 繧ｿ繧､繝槭・縺檎匱轣ｫ縺励◆繧峨∫峩霑・N 蛻・+ 蟆代＠蜑阪・繝｡繝・そ繝ｼ繧ｸ繧貞叙蠕・
 *   3. summarizer 縺ｧ隕∫ｴ・+ 隴ｰ隲也噪縺ｪ繝｡繝・そ繝ｼ繧ｸ繧・pickup
 *   4. GitHub Discussions 縺ｫ謚慕ｨｿ (GITHUB_TOKEN + 蟇ｾ雎｡繝ｪ繝昴ず繝医Μ險ｭ螳壽凾)
 *   5. chat_summaries 繝・・繝悶Ν縺ｫ繧ゆｿ晏ｭ・(豌ｸ邯壼喧)
 *
 * 霑ｽ蜉縺ｧ謚慕ｨｿ縺後≠繧後・譌｢蟄倥・繧ｿ繧､繝槭・繧偵Μ繧ｻ繝・ヨ縺励√せ繝ｬ繝・ラ蛻・∪縺ｨ繧√※隕∫ｴ・☆繧九・
 */

import { randomUUID } from "crypto";
import { chatMessageRepo, chatSummaryRepo, monitorRepo } from "../db/repository.js";
import { summarizeMessages } from "./summarizer.js";
import {
  discussionSessionStore,
  type DiscussionSession,
} from "./mode-state.js";
import { publishToGithubDiscussion } from "./github-discussion.js";
import { sendChannelNotice } from "./chat-reply.js";

export interface DiscussionModeInput {
  monitorId: string;
  workspaceId: string;
  delayMinutes: number;
  /** 隕∫ｴ・ｯｾ雎｡譛滄俣縺ｮ荳矩剞繧定ｨ育ｮ励☆繧九◆繧√↓菴ｿ縺・(謚慕ｨｿ譎ょ綾) */
  postedAt: Date;
}

/** 譁ｰ隕乗兜遞ｿ繧貞女縺代※繧ｿ繧､繝槭・繧・debounce 逋ｻ骭ｲ縺吶ｋ */
export function scheduleDiscussionDigest(input: DiscussionModeInput): DiscussionSession {
  const delayMs = Math.max(1, input.delayMinutes) * 60_000;
  const now = new Date();
  const scheduledAt = new Date(now.getTime() + delayMs);

  const existing = discussionSessionStore.findByMonitor(input.monitorId);
  if (existing && (existing.status === "pending" || existing.status === "failed")) {
    // 譌｢蟄倥ち繧､繝槭・繧偵Μ繧ｻ繝・ヨ
    if (existing._timer) clearTimeout(existing._timer);
    const updated = discussionSessionStore.update(existing.id, {
      scheduledAt,
      status: "pending",
      errorReason: undefined,
    });
    const timer = setTimeout(() => {
      void runDiscussionDigest(existing.id).catch((err) => {
        console.error("[discussion-mode] timer error:", err);
      });
    }, delayMs);
    if (updated) {
      discussionSessionStore.update(existing.id, { _timer: timer });
    }
    return updated!;
  }

  const windowStart = new Date(input.postedAt.getTime() - delayMs * 2);
  const session = discussionSessionStore.create({
    monitorId: input.monitorId,
    workspaceId: input.workspaceId,
    scheduledAt,
    windowStart,
    status: "pending",
  });

  const timer = setTimeout(() => {
    void runDiscussionDigest(session.id).catch((err) => {
      console.error("[discussion-mode] timer error:", err);
    });
  }, delayMs);
  discussionSessionStore.update(session.id, { _timer: timer });

  return session;
}

/** 繧ｿ繧､繝槭・逋ｺ轣ｫ or 謇句虚繝医Μ繧ｬ譎ゅ・螳溷・逅・*/
export async function runDiscussionDigest(sessionId: string): Promise<{
  messageCount: number;
  summaryId?: string;
  githubUrl?: string;
} | null> {
  const session = discussionSessionStore.findById(sessionId);
  if (!session) return null;

  const monitor = await monitorRepo.findById(session.monitorId);
  if (!monitor) {
    discussionSessionStore.update(sessionId, {
      status: "failed",
      errorReason: "繝｢繝九ち縺瑚ｦ九▽縺九ｊ縺ｾ縺帙ｓ",
    });
    return null;
  }

  discussionSessionStore.update(sessionId, { status: "summarizing" });

  try {
    const periodEnd = new Date();
    const messages = await chatMessageRepo.findByMonitorIdInRange(
      session.monitorId,
      session.windowStart,
      periodEnd
    );

    // 隴ｰ隲也噪縺ｪ蜀・ｮｹ繧・feature 縺吶ｋ: 隍・焚莠ｺ縺ｮ蠢憺・ / 荳螳壹・髟ｷ縺・/ 逍大撫譁・ｒ蜷ｫ繧
    const featured = messages.filter(isDiscussionMessage);
    const targetMessages = featured.length >= 2 ? featured : messages;

    const result = summarizeMessages(targetMessages);
    const summaryId = randomUUID();

    await chatSummaryRepo.create({
      id: summaryId,
      monitorId: session.monitorId,
      workspaceId: session.workspaceId,
      periodStart: session.windowStart,
      periodEnd,
      summary: result.summary,
      highlights: JSON.stringify(result.highlights),
      messageCount: result.messageCount,
      createdBy: "system",
      createdAt: new Date(),
    });

    // GitHub Discussion 縺ｸ publish
    let githubUrl: string | undefined;
    if (monitor.githubRepo) {
      discussionSessionStore.update(sessionId, { status: "publishing" });
      try {
        const res = await publishToGithubDiscussion({
          repo: monitor.githubRepo,
          categoryId: monitor.githubDiscussionCategoryId ?? null,
          title: "[" + monitor.channelName + "] " + session.windowStart.toISOString().slice(0, 16) + " - " + periodEnd.toISOString().slice(0, 16) + " discussion summary",
          body: buildGithubDiscussionBody({
            channelName: monitor.channelName,
            platform: monitor.platform,
            summary: result.summary,
            highlights: result.highlights,
            messageCount: result.messageCount,
            periodStart: session.windowStart,
            periodEnd,
          }),
        });
        githubUrl = res?.url;
      } catch (err) {
        // publish 螟ｱ謨励〒繧・summary 菴懈・閾ｪ菴薙・謌仙粥謇ｱ縺・《ession 縺ｯ failed 縺ｫ
        discussionSessionStore.update(sessionId, {
          status: "failed",
          errorReason:
            err instanceof Error
              ? ("GitHub publish failed: " + err.message)
              : "GitHub publish failed",
        });
        return { messageCount: result.messageCount, summaryId };
      }
    }

    discussionSessionStore.update(sessionId, {
      status: "summarizing",
      lastPublishedUrl: githubUrl,
    });
    // 豁｣蟶ｸ邨ゆｺ・＠縺溘ｉ遐ｴ譽・
    await sendChannelNotice({
      monitorId: session.monitorId,
      platform: monitor.platform as "slack" | "discord",
      channelId: monitor.channelId,
      text: buildChannelDigestNotice({
        channelName: monitor.channelName,
        messageCount: result.messageCount,
        summary: result.summary,
        githubUrl,
      }),
    }).catch((err) => {
      console.error("[discussion-mode] channel notice failed:", err);
    });
    discussionSessionStore.remove(sessionId);

    return { messageCount: result.messageCount, summaryId, githubUrl };
  } catch (err) {
    discussionSessionStore.update(sessionId, {
      status: "failed",
      errorReason: err instanceof Error ? err.message : String(err),
    });
    return null;
  }
}

/** 隴ｰ隲也噪繝｡繝・そ繝ｼ繧ｸ縺ｮ繝輔ぅ繝ｫ繧ｿ: 荳螳夐聞 or 逍大撫隨ｦ or 蜿榊ｿ懊Ρ繝ｼ繝・*/
function isDiscussionMessage(m: { text: string }): boolean {
  const t = m.text;
  if (t.length >= 30) return true;
  if (t.includes("?") || t.includes("？")) return true;
  const kw = ["どう", "議論", "提案", "なぜ", "べき", "案"];
  return kw.some((k) => t.includes(k));
}

function buildGithubDiscussionBody(args: {
  channelName: string;
  platform: string;
  summary: string;
  highlights: ReturnType<typeof summarizeMessages>["highlights"];
  messageCount: number;
  periodStart: Date;
  periodEnd: Date;
}): string {
  const lines: string[] = [];
  lines.push("> **Channel**: " + args.channelName + " (" + args.platform + ")");
  lines.push("> **Period**: " + args.periodStart.toISOString() + " - " + args.periodEnd.toISOString());
  lines.push("> **Messages**: " + args.messageCount);
  lines.push("");
  lines.push("## Summary");
  lines.push(args.summary);
  if (args.highlights.participants.length > 0) {
    lines.push("");
    lines.push("## Participants");
    for (const p of args.highlights.participants.slice(0, 10)) {
      lines.push("- " + p.authorName + ": " + p.messageCount + " messages");
    }
  }
  if (args.highlights.topKeywords.length > 0) {
    lines.push("");
    lines.push("## Top Keywords");
    lines.push(
      args.highlights.topKeywords
        .slice(0, 10)
        .map((k) => "`" + k.keyword + "` x" + k.count)
        .join(" / ")
    );
  }
  if (args.highlights.topMessages.length > 0) {
    lines.push("");
    lines.push("## Highlight Messages");
    for (const m of args.highlights.topMessages) {
      lines.push("- **" + m.authorName + "** (" + m.postedAt + "): " + m.text);
    }
  }
  lines.push("");
  lines.push("_Auto-generated by Discutere discussion mode._");
  return lines.join("\n");
}

/** 繧ｻ繝・す繝ｧ繝ｳ繧呈焔蜍輔く繝｣繝ｳ繧ｻ繝ｫ */
export function dismissDiscussionSession(sessionId: string): boolean {
  return discussionSessionStore.remove(sessionId);
}

/** 蜊ｳ譎ょｮ溯｡・(繧ｿ繧､繝槭・蠕・■繧偵せ繧ｭ繝・・) */
export async function flushDiscussionSession(
  sessionId: string
): Promise<Awaited<ReturnType<typeof runDiscussionDigest>>> {
  const session = discussionSessionStore.findById(sessionId);
  if (!session) return null;
  if (session._timer) clearTimeout(session._timer);
  discussionSessionStore.update(sessionId, { _timer: undefined });
  return runDiscussionDigest(sessionId);
}

function buildChannelDigestNotice(args: {
  channelName: string;
  messageCount: number;
  summary: string;
  githubUrl?: string;
}): string {
  const header = "[Discussion Summary] " + args.channelName + " / " + args.messageCount + " messages";
  const body = args.summary.length > 500 ? (args.summary.slice(0, 500) + "...") : args.summary;
  const link = args.githubUrl ? ("\nGitHub Discussion: " + args.githubUrl) : "";
  return header + "\n" + body + link;
}
