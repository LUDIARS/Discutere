import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";

import { scoreText } from "../../src/analysis/affect-score.js";
import { cascadeSentiment } from "../../src/crawler/sentiment/cascade.js";
import { textToVector } from "../../src/flow/sentiment-vector.js";
import { buildVector, extractTextFeatures } from "@ludiars/sentiment-core";
import { GOLDEN_TEXTS } from "./golden-inputs.js";

interface GoldenDigests {
  textToVector: string;
  scoreText: string;
  cascadeTier0: string;
  gameVector: number[];
}

function digest(value: unknown): string {
  return createHash("sha256").update(JSON.stringify(value)).digest("hex");
}

const expected = JSON.parse(
  readFileSync(new URL("./golden.json", import.meta.url), "utf8"),
) as GoldenDigests;
const cascade = await Promise.all(GOLDEN_TEXTS.map((text) => cascadeSentiment(text, undefined, {})));
const aggregateInput = JSON.parse(
  readFileSync(new URL("./aggregate-input.json", import.meta.url), "utf8"),
) as { records: Array<{ text: string; meta?: { voted_up?: boolean } }> };

assert.equal(digest(GOLDEN_TEXTS.map((text) => textToVector(text))), expected.textToVector);
assert.equal(digest(GOLDEN_TEXTS.map((text) => scoreText(text))), expected.scoreText);
assert.equal(digest(cascade), expected.cascadeTier0);
assert.deepEqual(
  buildVector(aggregateInput.records.map((record) => extractTextFeatures(record.text, {
    votedUp: record.meta?.voted_up,
  }))),
  expected.gameVector,
);

console.log("ok sentiment-core 50-text golden digest");
