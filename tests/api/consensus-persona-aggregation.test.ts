import assert from "node:assert/strict";

import { aggregatePersonaConsensus, ratioOfAgreed } from "../../src/api/consensus-persona-aggregation.js";

const personas = aggregatePersonaConsensus([
  {
    id: "u2", personaId: "persona-a", displayName: "A", traits: ["慎重"], content: "second", postedAt: 20,
    agree: false, score: 3, reasoning: "no", opinionScore: 4,
  },
  {
    id: "u1", personaId: "persona-a", displayName: "A", traits: ["慎重"], content: "first", postedAt: 10,
    agree: true, score: 7, reasoning: "yes", opinionScore: 2,
  },
  {
    id: "u3", personaId: "persona-b", displayName: "B", traits: ["新規性重視"], content: "pending", postedAt: 15,
    agree: null, score: null, reasoning: null, opinionScore: 1,
  },
]);

assert.equal(personas.length, 2);
assert.deepEqual(personas[0], {
  personaId: "persona-a",
  displayName: "A",
  traits: ["慎重"],
  utterances: [
    { id: "u1", content: "first", postedAt: 10, agree: true, score: 7, reasoning: "yes" },
    { id: "u2", content: "second", postedAt: 20, agree: false, score: 3, reasoning: "no" },
  ],
  opinionScore: 6,
  consensusScore: 10,
  agreeRatio: 0.5,
});
assert.equal(personas[1].agreeRatio, 0);
assert.equal(ratioOfAgreed([{ agree: null }]), 0);

console.log("consensus-persona-aggregation.test.ts: all passed");
