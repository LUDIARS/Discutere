/**
 * 自動シード議論スケジューラ (#64/#65)。
 *
 * 定期的に種 (ジャンル / ストアトレンド) から headless 議論を 1 つ立てる。
 * 駆動は server で常駐している facilitator に任せる (= ここは「種まき」だけ)。
 * 同時に開く headless 議論を maxConcurrent で抑え、 暴走を防ぐ。
 */

import type { createCore } from "../core/index.js";
import type { PersonasRepo } from "../persona-engine/db/personas-repo.js";
import type { Logger } from "../persona-engine/types.js";
import { buildGenreTopic, pickGenreSeed, type SeedTopic } from "./genres.js";
import { fetchStoreTrendTopics } from "./store-trends.js";
import { seedHeadlessDiscussion } from "./seed.js";

type Core = ReturnType<typeof createCore>;

export interface AutoSeedSchedulerDeps {
  core: Core;
  personas: PersonasRepo;
  workspaceId: string;
  config: {
    intervalMs: number;
    maxConcurrent: number;
    sources: string[];
  };
  logger: Logger;
}

export interface AutoSeedScheduler {
  start(): void;
  stop(): void;
  /** 1 回分を即時実行 (手動トリガ / テスト)。 立てた gap を返す (skip 時は null)。 */
  trigger(): Promise<{ gapId: string; title: string } | null>;
}

export function createAutoSeedScheduler(deps: AutoSeedSchedulerDeps): AutoSeedScheduler {
  let timer: NodeJS.Timeout | null = null;
  let running = false;
  let rotation = 0; // ジャンル / source を順に回すためのカウンタ
  const sources = deps.config.sources.length ? deps.config.sources : ["genre"];

  function openHeadlessCount(): number {
    const row = deps.core.client.raw
      .prepare(
        `SELECT COUNT(*) AS n
           FROM sessions s
           JOIN design_gaps g
             ON g.id = SUBSTR(s.title, LENGTH('discussion-of-gap:') + 1)
          WHERE s.workspace_id = ?
            AND s.title LIKE 'discussion-of-gap:%'
            AND s.scene LIKE 'gap:%'
            AND (g.status IS NULL OR g.status NOT IN ('closed', 'converged'))`
      )
      .get(deps.workspaceId) as { n: number };
    return row.n;
  }

  function hasOpenGapWithTitle(title: string): boolean {
    const row = deps.core.client.raw
      .prepare(
        "SELECT 1 FROM design_gaps WHERE workspace_id = ? AND title = ? AND (status IS NULL OR status NOT IN ('closed', 'converged')) LIMIT 1"
      )
      .get(deps.workspaceId, title) as { 1: number } | undefined;
    return !!row;
  }

  async function nextTopic(): Promise<{ topic: SeedTopic; origin: string } | null> {
    const source = sources[rotation % sources.length];
    rotation += 1;
    if (source === "store-trend") {
      const topics = await fetchStoreTrendTopics(8);
      const fresh = topics.find((t) => !hasOpenGapWithTitle(t.title));
      return fresh ? { topic: fresh, origin: "auto:store-trend" } : null;
    }
    // default: genre — 被らないジャンルを最大 17 周ぶん探す
    for (let i = 0; i < 17; i++) {
      const topic = buildGenreTopic(pickGenreSeed(rotation + i));
      if (!hasOpenGapWithTitle(topic.title)) return { topic, origin: "auto:genre" };
    }
    return null;
  }

  async function trigger(): Promise<{ gapId: string; title: string } | null> {
    if (openHeadlessCount() >= deps.config.maxConcurrent) {
      deps.logger.info(
        { open: openHeadlessCount(), max: deps.config.maxConcurrent },
        "auto-seed skipped (maxConcurrent reached)"
      );
      return null;
    }
    const next = await nextTopic();
    if (!next) {
      deps.logger.info({}, "auto-seed skipped (no fresh topic)");
      return null;
    }
    const seeded = seedHeadlessDiscussion({
      core: deps.core,
      personas: deps.personas,
      workspaceId: deps.workspaceId,
      topic: next.topic,
      origin: next.origin,
    });
    deps.logger.info(
      { gap_id: seeded.gapId, title: next.topic.title, origin: next.origin },
      "auto-seed started headless discussion"
    );
    return { gapId: seeded.gapId, title: next.topic.title };
  }

  return {
    start(): void {
      if (timer) return;
      timer = setInterval(() => {
        if (running) return;
        running = true;
        trigger()
          .catch((err) => deps.logger.warn({ err: (err as Error).message }, "auto-seed tick failed"))
          .finally(() => {
            running = false;
          });
      }, deps.config.intervalMs);
    },
    stop(): void {
      if (timer) clearInterval(timer);
      timer = null;
    },
    trigger,
  };
}
