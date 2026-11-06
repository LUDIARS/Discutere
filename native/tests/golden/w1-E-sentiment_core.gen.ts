// @ludiars/sentiment-core (Node 実装) の入出力を記録する。実行: npx tsx native/tests/golden/w1-E-sentiment_core.gen.ts
// 出力: native/tests/fixtures/sentiment_core/*.json
import fs from "node:fs";
import path from "node:path";
import {
  VECTOR_SPEC,
  buildTargetVector,
  buildVector,
  cascadeSentiment,
  computeDesignGap,
  cosine,
  evaluateSpeakers,
  extractTextFeatures,
  loadAffectVocabulary,
  affectVocabularyVersion,
  scalarProjection,
  scoreText,
  textToVector,
  weightedMean,
} from "@ludiars/sentiment-core";
import { GOLDEN_TEXTS } from "../../../tests/sentiment-core/golden-inputs.js";

const dir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "sentiment_core");
fs.mkdirSync(dir, { recursive: true });
const write = (name: string, value: unknown) =>
  fs.writeFileSync(path.join(dir, name), JSON.stringify(value, null, 2) + "\n");

const extra = [
  "AWESOME Combat!!", "ÉPIC fail", "ＧＯＯＤ ｇａｍｅ", "great great great", "not good, not bad, boring and fun",
  "神ゲー!!!!!!!!", "a".repeat(400) + " love", "😀 最高", "mixed: love hate love",
];
const texts = [...GOLDEN_TEXTS, ...extra];
const signals = [undefined, { votedUp: true }, { votedUp: false }, { upvotes: 0 }, { upvotes: 999 }, { votedUp: true, upvotes: 10 }, { votedUp: false, upvotes: 5000 }];

const textCases = texts.map((text) => ({
  text,
  vector: textToVector(text),
  score: scoreText(text),
  features: extractTextFeatures(text),
}));
const cascadeCases: unknown[] = [];
for (const text of texts) {
  for (const signal of signals) {
    cascadeCases.push({ text, signal: signal ?? null, result: await cascadeSentiment(text, signal, {}) });
  }
}
// LLM 段: 記録した応答で 1 段/2 段の振る舞いを確かめる
const llmResponses = [
  '{"polarity":"positive","confidence":0.9}',
  'sure: {"polarity":"negative","confidence":0.7} done',
  '{"polarity":"neutral","confidence":"0.8"}',
  '{"polarity":"positive","confidence":2}',
  '{"polarity":"positive"}',
  '{"polarity":"bad","confidence":0.9}',
  'no json here',
  '{"polarity":"positive","confidence":0.5}',
  '{broken',
  '{"polarity":"negative","confidence":true}',
  '{"polarity":"negative","confidence":null}',
];
const llmCases: unknown[] = [];
for (const local of [...llmResponses, null]) {
  for (const main of [llmResponses[0], llmResponses[6], null]) {
    const prompts: string[] = [];
    const client = (text: string | null) => text === null ? undefined : {
      invoke: async (req: { system: string; prompt: string; maxTokens: number }) => {
        prompts.push(JSON.stringify(req));
        return { ok: true as const, text };
      },
    };
    const result = await cascadeSentiment("so-so " + "x".repeat(320), undefined, { local: client(local), main: client(main) });
    llmCases.push({ local, main, result, prompts });
  }
}

const aggregate = JSON.parse(fs.readFileSync("tests/sentiment-core/aggregate-input.json", "utf8")) as {
  records: Array<{ text: string; meta?: { voted_up?: boolean } }>;
};
const gameVector = buildVector(aggregate.records.map((r) => extractTextFeatures(r.text, { votedUp: r.meta?.voted_up })));

const targets = [
  [],
  [{ intended_valence: "positive", intended_emotions: ["joy"], intended_aspects: ["fun"] }],
  [{ intended_valence: "negative" }, { intended_valence: "positive" }, { intended_valence: "mixed" }],
  [{ intended_aspects: ["story", "unknown"], intended_emotions: ["fear", "nope"] }, { intended_valence: "negative" }],
].map((mechanics) => {
  const target = buildTargetVector(mechanics);
  return { mechanics, target, gap: computeDesignGap(textToVector("最高に面白い!"), target) };
});

const mk = (id: string, items: Array<[string, string | null, number?]>) => ({
  speakerId: id,
  source: "steam",
  opinions: items.map(([text, gameSlug, weight]) => ({ text, gameSlug, ...(weight !== undefined ? { weight } : {}) })),
});
const speakers = [
  mk("few", [["good", "a"]]),
  mk("one-game", Array.from({ length: 10 }, () => ["good fun", "a"] as [string, string])),
  mk("all-pos", [...Array.from({ length: 5 }, () => ["amazing", "a"] as [string, string]), ...Array.from({ length: 5 }, () => ["great", "b"] as [string, string])]),
  mk("all-neg", [...Array.from({ length: 5 }, () => ["boring", "a"] as [string, string]), ...Array.from({ length: 5 }, () => ["terrible", "b"] as [string, string])]),
  mk("mixed", [
    ...Array.from({ length: 4 }, () => ["amazing story, beautiful graphics", "a", 2] as [string, string, number]),
    ...Array.from({ length: 4 }, () => ["boring grind, lag and crash", "b"] as [string, string]),
    ["普通", null], ["つまらない", null],
  ]),
  mk("flat", [...Array.from({ length: 5 }, () => ["xyz", "a"] as [string, string]), ...Array.from({ length: 5 }, () => ["xyz", "b"] as [string, string])]),
];
const speakerResult = evaluateSpeakers(speakers);
const speakerResult2 = evaluateSpeakers(speakers, { minOpinions: 2, polarityEps: 0.2, gameGapEps: 0.5 });

const left = textToVector("love");
const right = textToVector("boring grind");
write("text.json", { spec: VECTOR_SPEC, cases: textCases });
write("cascade.json", { tier0: cascadeCases, llm: llmCases });
write("misc.json", {
  gameVector,
  targets,
  speakers: { input: speakers, defaults: speakerResult, custom: speakerResult2 },
  math: { left, right, cosine: cosine(left, right), projection: scalarProjection(left, right), mean: weightedMean([left, right], [1, 3]) },
  vocabulary: { version: affectVocabularyVersion(), entries: loadAffectVocabulary() },
  goldenTextCount: GOLDEN_TEXTS.length,
});
console.log(`wrote ${textCases.length} text cases, ${cascadeCases.length} cascade cases`);
