/**
 * チャンネルモード「議論」の処理器。
 *
 * フロー:
 *   1. メッセージ投稿を受信すると、チャンネル単位で「N 分後の要約タイマー」を登録 (debounce)
 *   2. タイマーが発火したら、直近 N 分 + 少し前のメッセージを取得
 *   3. summarizer で要約 + 議論的なメッセージを pickup
 *   4. GitHub Discussions に投稿 (GITHUB_TOKEN + 対象リポジトリ設定時)
 *   5. chat_summaries テーブルにも保存 (永続化)
 *
 * 追加で投稿があれば既存のタイマーをリセットし、スレッド分まとめて要約する。
 */

import { randomUUID } from "crypto";
import { chatMessageRepo, chatSummaryRepo, monitorRepo } from "../db/repository.js";
import { summarizeMessages } from "./summarizer.js";
import {
  discussionSessionStore,
  type DiscussionSession,
} from "./mode-state.js";
import { publishToGithubDiscussion } from "./github-discussion.js";

export interface DiscussionModeInput {
  monitorId: string;
  workspaceId: string;
  delayMinutes: number;
  /** 要約対象期間の下限を計算するために使う (投稿時刻) */
  postedAt: Date;
}

/** 新規投稿を受けてタイマーを debounce 登録する */
export function scheduleDiscussionDigest(input: DiscussionModeInput): DiscussionSession {
  const delayMs = Math.max(1, input.delayMinutes) * 60_000;
  const now = new Date();
  const scheduledAt = new Date(now.getTime() + delayMs);

  const existing = discussionSessionStore.findByMonitor(input.monitorId);
  if (existing && (existing.status === "pending" || existing.status === "failed")) {
    // 既存タイマーをリセット
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

/** タイマー発火 or 手動トリガ時の実処理 */
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
      errorReason: "モニタが見つかりません",
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

    // 議論的な内容を feature する: 複数人の応酬 / 一定の長さ / 疑問文を含む
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

    // GitHub Discussion へ publish
    let githubUrl: string | undefined;
    if (monitor.githubRepo) {
      discussionSessionStore.update(sessionId, { status: "publishing" });
      try {
        const res = await publishToGithubDiscussion({
          repo: monitor.githubRepo,
          categoryId: monitor.githubDiscussionCategoryId ?? null,
          title: `[${monitor.channelName}] ${session.windowStart
            .toISOString()
            .slice(0, 16)} 〜 ${periodEnd.toISOString().slice(0, 16)} の議論要約`,
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
        // publish 失敗でも summary 作成自体は成功扱い、session は failed に
        discussionSessionStore.update(sessionId, {
          status: "failed",
          errorReason:
            err instanceof Error
              ? `GitHub publish 失敗: ${err.message}`
              : "GitHub publish 失敗",
        });
        return { messageCount: result.messageCount, summaryId };
      }
    }

    discussionSessionStore.update(sessionId, {
      status: "summarizing",
      lastPublishedUrl: githubUrl,
    });
    // 正常終了したら破棄
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

/** 議論的メッセージのフィルタ: 一定長 or 疑問符 or 反応ワード */
function isDiscussionMessage(m: { text: string }): boolean {
  const t = m.text;
  if (t.length >= 30) return true;
  if (/[?？]/.test(t)) return true;
  if (/(どう思う|意見|議論|提案|案|検討|賛成|反対|思う|べき)/.test(t)) return true;
  return false;
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
  lines.push(`> **チャンネル**: ${args.channelName} (${args.platform})`);
  lines.push(
    `> **期間**: ${args.periodStart.toISOString()} 〜 ${args.periodEnd.toISOString()}`
  );
  lines.push(`> **メッセージ数**: ${args.messageCount}`);
  lines.push("");
  lines.push("## 要約");
  lines.push(args.summary);
  if (args.highlights.participants.length > 0) {
    lines.push("");
    lines.push("## 参加者");
    for (const p of args.highlights.participants.slice(0, 10)) {
      lines.push(`- ${p.authorName} — ${p.messageCount} 発言`);
    }
  }
  if (args.highlights.topKeywords.length > 0) {
    lines.push("");
    lines.push("## 頻出キーワード");
    lines.push(
      args.highlights.topKeywords
        .slice(0, 10)
        .map((k) => `\`${k.keyword}\` ×${k.count}`)
        .join(" / ")
    );
  }
  if (args.highlights.topMessages.length > 0) {
    lines.push("");
    lines.push("## 代表メッセージ");
    for (const m of args.highlights.topMessages) {
      lines.push(`- **${m.authorName}** (${m.postedAt}): ${m.text}`);
    }
  }
  lines.push("");
  lines.push("_このディスカッションは Discutere が議論モードで自動生成しました。_");
  return lines.join("\n");
}

/** セッションを手動キャンセル */
export function dismissDiscussionSession(sessionId: string): boolean {
  return discussionSessionStore.remove(sessionId);
}

/** 即時実行 (タイマー待ちをスキップ) */
export async function flushDiscussionSession(
  sessionId: string
): Promise<Awaited<ReturnType<typeof runDiscussionDigest>>> {
  const session = discussionSessionStore.findById(sessionId);
  if (!session) return null;
  if (session._timer) clearTimeout(session._timer);
  discussionSessionStore.update(sessionId, { _timer: undefined });
  return runDiscussionDigest(sessionId);
}
