/**
 * C2 ペルソナ生成エンジン: survey / generateSyntheticPersonas / estimatePopulations。
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

const TMP_DIR = path.resolve(".tmp/flow-test");
fs.mkdirSync(TMP_DIR, { recursive: true });
process.env.DATABASE_PATH = path.join(TMP_DIR, "persona-survey.db");
try {
  fs.rmSync(process.env.DATABASE_PATH, { force: true });
} catch {
  /* noop */
}

const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
_resetFlowDb();

const survey = await import("../../src/flow/survey.js");
const { generateSyntheticPersonas } = await import("../../src/flow/persona-survey.js");
const { estimatePopulations } = await import("../../src/flow/persona-populations.js");
const { insertPoolPersona, listPoolPersonas } = await import("../../src/flow/persona-pool.js");
const { textToVector } = await import("../../src/flow/sentiment-vector.js");

// ── survey スキーマ ─────────────────────────────────────────────
{
  let seed = 42;
  const rng = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
  const p = survey.randomProfile(rng);
  assert.ok(p.genres.length >= 1 && p.genres.length <= 3, "genres 1..3");
  assert.ok(survey.PLAY_STYLES.includes(p.playStyles[0]), "playStyle 妥当");
  // ジャンル一致は不一致より play 確率が高い
  const prof: survey.SurveyProfile = {
    genres: ["roguelike"],
    playStyles: ["hardcore"],
    emotionSeeks: ["達成感"],
    ageBand: "20s",
    spending: "light",
  };
  assert.ok(
    survey.playProbability(prof, "roguelike") > survey.playProbability(prof, "gacha"),
    "ジャンル一致で play 確率が上がる"
  );
  console.log("  [ok] C2 survey: randomProfile / playProbability");
}

// ── generateSyntheticPersonas (mock LLM, 強制プレイ) ─────────────
{
  const mockLlm = {
    async invoke() {
      return { ok: true as const, text: "great fun awesome 最高に楽しい" };
    },
  };
  const rng = () => 0.01; // 常にプレイ (rng < playProbability)
  const profile: survey.SurveyProfile = {
    genres: ["roguelike"],
    playStyles: ["hardcore"],
    emotionSeeks: ["達成感"],
    ageBand: "20s",
    spending: "medium",
  };
  const res = await generateSyntheticPersonas({
    count: 2,
    games: [
      { slug: "g-a", title: "Game A", genre: "roguelike" },
      { slug: "g-b", title: "Game B", genre: "roguelike" },
    ],
    llm: mockLlm,
    rng,
    profiles: [profile, profile],
  });
  assert.equal(res.personas.length, 2, "合成 2 体保存");
  assert.equal(res.personas[0].origin, "generated", "origin=generated");
  assert.equal(res.personas[0].learningSource, "survey", "learningSource=survey");
  assert.ok(res.breakdown[0].played >= 1, "プレイ感想あり");
  assert.equal(listPoolPersonas({ origin: "generated" }).length, 2, "プールに合成 2 体");
  console.log("  [ok] C2 generateSyntheticPersonas (mock LLM)");
}

// ── estimatePopulations (案A: 実分布突合) ───────────────────────
{
  // 実分布 (adopted) を合成と近い affect で 1 体投入 → クラスタが「大」判定される
  insertPoolPersona({
    id: "real-1",
    name: "論者#real",
    role: "opinion",
    speechStyle: "",
    traits: [],
    affectVector: textToVector("great fun awesome 最高に楽しい"),
    origin: "adopted",
    parentIds: [],
    sourceSpeakerId: "ext:steam:real1",
  });
  const report = estimatePopulations({ simThreshold: 0.8, bigRatio: 0.15 });
  assert.equal(report.realCount, 1, "実分布 1");
  assert.ok(report.syntheticCount >= 2, "合成 ≥2");
  assert.ok(report.clusters.length >= 1, "クラスタあり");
  const big = report.clusters.find((c) => c.verdict === "大");
  assert.ok(big, "実近傍を持つクラスタは大判定");
  console.log("  [ok] C2 estimatePopulations (実分布突合で母数大小)");
}

console.log("persona-survey tests: all passed");
