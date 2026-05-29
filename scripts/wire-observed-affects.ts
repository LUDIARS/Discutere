/**
 * 観測 affect 配線 (Axis2 → DesignGap)
 *
 *   data/games/<slug>.sentiment.json の discussion clusters を Affect 語彙 key に写像し、
 *   emotion セッションの Utterance + 承認済み affect proposal (translation_proposals) として投入。
 *   その後 detectGaps を実行して intended_affect × observed のズレを DesignGap 化する。
 *
 *   npm run wire:affects                 # data/games/*.sentiment.json 全件
 *   npm run wire:affects -- vampire-survivors
 *
 *   原文(レビュー本文)は投入しない (summary-only)。utterance.raw_content は合成サマリラベル。
 */
import fs from "node:fs";
import path from "node:path";
import { randomUUID } from "node:crypto";
import { createCore } from "../src/core/index.js";
import { approveProposal } from "../src/core/bridge/translation/review-handler.js";
import { detectGaps } from "../src/core/bridge/gap/detector.js";

const WS = "knowledge";
const GAMES = path.resolve("data/games");

// (aspect, sentiment) → Affect 語彙 key
const ASPECT_MAP: Record<string, { positive: string; negative: string }> = {
  fun: { positive: "game_feel", negative: "boredom" },
  difficulty: { positive: "mastery", negative: "frustration" },
  content: { positive: "collection", negative: "monotony" },
  price_value: { positive: "strategic", negative: "unfair_monetization" },
  performance: { positive: "game_feel", negative: "frustration" },
  story: { positive: "narrative", negative: "boredom" },
  graphics: { positive: "wonder", negative: "confusion" },
  replayability: { positive: "mastery", negative: "monotony" },
};
const affectForCluster = (c: any): string | null => {
  const m = ASPECT_MAP[c.topic_aspect];
  if (!m) return null;
  if (c.sentiment === "negative") return m.negative;
  if (c.sentiment === "positive") return m.positive;
  return m.positive; // neutral は positive 側に寄せる
};

function wireGame(core: any, slug: string, sent: any): { utterances: number; observed: string[] } {
  const sid = core.repos.session.create({ workspaceId: WS, title: `reception:${slug}`, startedAt: Date.now(), mode: "emotion" });
  const observed: string[] = [];
  for (const c of sent.clusters || []) {
    const key = affectForCluster(c);
    if (!key) continue;
    const uid = core.repos.utterance.create({
      workspaceId: WS, sessionId: sid,
      rawContent: `[${slug}] aspect=${c.topic_aspect} sentiment=${c.sentiment} (n=${c.size})`, // summary-only
      normalizedContent: `観測: ${key}`, postedAt: Date.now(),
    });
    const pid = randomUUID();
    const now = Date.now();
    core.client.raw.prepare(
      `INSERT INTO translation_proposals (id, workspace_id, utterance_id, proposal_type, target_name, confidence, pending_review, status, payload_json, created_at, updated_at)
       VALUES (?, ?, ?, 'affect', ?, ?, 1, 'pending', ?, ?, ?)`
    ).run(pid, WS, uid, key, c.score ?? 0.5, JSON.stringify({ source: slug, aspect: c.topic_aspect, sentiment: c.sentiment }), now, now);
    approveProposal(core, pid); // status=approved + affect 計測行
    observed.push(key);
  }
  return { utterances: observed.length, observed };
}

const onlySlug = process.argv.slice(2).find((a) => !a.startsWith("--"));
const files = fs.readdirSync(GAMES).filter((f) => f.endsWith(".sentiment.json"))
  .filter((f) => !onlySlug || f === `${onlySlug}.sentiment.json`);

const core = createCore();
const allObserved: string[] = [];
for (const f of files) {
  const slug = f.replace(/\.sentiment\.json$/, "");
  const sent = JSON.parse(fs.readFileSync(path.join(GAMES, f), "utf8").replace(/^﻿/, ""));
  const r = wireGame(core, slug, sent);
  allObserved.push(...r.observed);
  console.log(`wired ${slug}: utterances=${r.utterances} observed=[${[...new Set(r.observed)].join(", ")}]`);
}
const gaps = detectGaps(core, WS);
core.close();
console.log(`\nobserved 合計 ${allObserved.length} 件 / DesignGap 生成 ${gaps.length} 件`);
