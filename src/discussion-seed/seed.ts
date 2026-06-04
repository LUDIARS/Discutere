/**
 * Headless 議論のシード (#64)。
 *
 * Discord 投稿を起点とせず、 与えられた議題 (ジャンル種 / トレンド種) から
 * design gap + 議論 session (scene=gap:<gapId> = 非 discord = headless) を立て、
 * 進行役 (facilitator) が開幕の一言を置いて「登場」する。
 *
 * 立てた gap は既存の facilitator 機構 (拡張→止揚判定→収束) がそのまま回す。
 * postUtterance は headless scene では Discord に出ず、 学習データ (KG/DB) にのみ残る。
 */

import type { createCore } from "../core/index.js";
import type { PersonasRepo } from "../persona-engine/db/personas-repo.js";
import { FACILITATOR_PERSONA, FACILITATOR_PERSONA_ID } from "../persona-engine/facilitator/index.js";
import type { SeedTopic } from "./genres.js";

type Core = ReturnType<typeof createCore>;

export interface SeedHeadlessDiscussionArgs {
  core: Core;
  /** persona-engine.db を開いた PersonasRepo (進行役の登録に使う) */
  personas: PersonasRepo;
  workspaceId: string;
  topic: SeedTopic;
  /** 議論の出自タグ (evidence_json に残す。 例 "auto:genre" / "auto:store-trend") */
  origin?: string;
}

export interface SeededDiscussion {
  gapId: string;
  sessionId: string;
}

export function seedHeadlessDiscussion(args: SeedHeadlessDiscussionArgs): SeededDiscussion {
  const { core, workspaceId, topic } = args;

  // 1) design gap (headless = discord 紐付けの guild/channel を持たない)
  const gapId = core.repos.designGap.create({
    workspaceId,
    title: topic.title,
    description: topic.description,
    status: "open",
  });

  // 出自を evidence に残す (discord guild/channel は意図的に入れない = scene が gap: になる)。
  core.client.raw
    .prepare("UPDATE design_gaps SET evidence_json = ?, updated_at = ? WHERE id = ?")
    .run(JSON.stringify({ source: args.origin ?? "auto:headless" }), Date.now(), gapId);

  // 2) 議論 session (scene=gap:<gapId> → discussion-bridge は post を skip = headless)
  const sessionId = core.repos.session.create({
    workspaceId,
    title: `discussion-of-gap:${gapId}`,
    startedAt: Date.now(),
    scene: `gap:${gapId}`,
  } as never);

  // 3) 進行役を登録して「登場」 — 開幕の一言で議題を提示する。
  args.personas.insertOrIgnore({
    ...FACILITATOR_PERSONA,
    traits: [...FACILITATOR_PERSONA.traits],
  });
  core.repos.utterance.create({
    workspaceId,
    sessionId,
    speakerId: `persona:${FACILITATOR_PERSONA_ID}`,
    rawContent: buildOpening(topic),
    postedAt: Date.now(),
  });

  return { gapId, sessionId };
}

function buildOpening(topic: SeedTopic): string {
  return (
    `今日のお題は「${topic.title}」です。\n` +
    `${topic.description}\n` +
    `それぞれの立場から、 思うところを聞かせてください。 まずは率直な意見からどうぞ。`
  );
}
