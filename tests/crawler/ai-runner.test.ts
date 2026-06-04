import assert from "node:assert/strict";

import {
  createAiTrainingRunner,
  parseGameKG,
  parseJsonObject,
} from "../../src/crawler/index.js";
import { MockLLMClient } from "../../src/persona-engine/index.js";

{
  const parsed = parseJsonObject<{ ok: boolean }>("```json\n{\"ok\":true}\n```");
  assert.equal(parsed.ok, true);
  console.log("ok parse fenced json");
}

{
  const draft = {
    id: "Synthetic Game!",
    title: "Synthetic Game",
    workspace_id: "knowledge",
    sources: [
      {
        url: "https://example.com/synthetic",
        excerpt_policy: "fair-use-quote",
      },
    ],
    mechanics: [
      {
        name: "Resource Loop",
        description: "Collect, spend, and improve.",
        intends: "Create short-term planning pressure.",
        intended_affect: "agency",
      },
    ],
    aesthetics: [
      {
        name: "Rising mastery",
        description: "Repeated loops feel more controlled over time.",
      },
    ],
  };
  const finalKg = {
    ...draft,
    id: "Synthetic Game!",
    body: "# AI debate\n\nCritic tightened the mechanic and kept source policy.",
  };
  const llm = new MockLLMClient([
    () => JSON.stringify(draft),
    () => JSON.stringify({
      risks: ["Synthetic draft needs human review."],
      corrections: ["Slug should be normalized."],
      missing: [],
    }),
    () => JSON.stringify(finalKg),
  ]);
  const runner = createAiTrainingRunner({ llm });
  const result = await runner.run("Synthetic Game", { maxSources: 2 });

  assert.equal(llm.calls, 3);
  assert.equal(result.kg.id, "synthetic-game");
  assert.equal(result.kg.sources[0].excerpt_policy, "fair-use-quote");
  assert.equal(result.meta.sourcesUsed[0], "https://example.com/synthetic");
  assert.doesNotThrow(() => parseGameKG(result.markdown));
  console.log("ok ai training runner");
}

console.log("ai-runner.test.ts: all passed");
