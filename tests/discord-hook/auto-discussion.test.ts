import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";

import { createCore } from "../../src/core/index.js";
import {
  classifyDiscordMessage,
  startAutoDiscussionForDiscordMessage,
} from "../../src/discord-hook/auto-discussion.js";
import { MockLLMClient } from "../../src/persona-engine/index.js";

const dir = path.resolve(".tmp/discord-auto-discussion");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });

const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const workspaceId = "knowledge";
const sessionId = core.repos.session.create({
  workspaceId,
  title: "discord-session:g1:ch1",
  startedAt: Date.now(),
  scene: "discord:g1/ch1",
});
const utteranceId = core.repos.utterance.create({
  workspaceId,
  sessionId,
  speakerId: "u1",
  rawContent: "ヴァンサバに必要な要素とは何か？",
  postedAt: Date.now(),
});

{
  const result = await startAutoDiscussionForDiscordMessage(core, {
    workspaceId,
    guildId: "g1",
    channelId: "ch1",
    sessionId,
    utteranceId,
    authorId: "u1",
    content: "ヴァンサバに必要な要素とは何か？",
  });
  assert.equal(result.started, true);
  assert.ok(result.gapId);

  const gap = core.client.raw
    .prepare("SELECT title, evidence_json FROM design_gaps WHERE id = ?")
    .get(result.gapId) as { title: string; evidence_json: string };
  assert.ok(gap.title.includes("Discord議題"));
  assert.ok(JSON.parse(gap.evidence_json).utteranceIds.includes(utteranceId));

  const event = core.client.raw
    .prepare("SELECT event_type FROM events WHERE event_type = 'DesignGapDetected' ORDER BY created_at DESC LIMIT 1")
    .get() as { event_type: string } | undefined;
  assert.equal(event?.event_type, "DesignGapDetected");
  console.log("ok auto discussion starts from fallback classification");
}

{
  const before = core.client.raw.prepare("SELECT COUNT(*) AS n FROM design_gaps").get() as { n: number };
  const result = await startAutoDiscussionForDiscordMessage(core, {
    workspaceId,
    guildId: "g1",
    channelId: "ch1",
    sessionId,
    utteranceId,
    authorId: "u1",
    content: "ヴァンサバに必要な要素とは何か？",
  });
  const after = core.client.raw.prepare("SELECT COUNT(*) AS n FROM design_gaps").get() as { n: number };
  assert.equal(result.started, false);
  assert.equal(after.n, before.n);
  console.log("ok auto discussion does not duplicate by utterance evidence");
}

{
  const llm = new MockLLMClient([
    () =>
      JSON.stringify({
        action: "start_discussion",
        category: "opinion",
        title: "LLM分類議題",
        description: "LLMで分類した議題",
        reason: "mock",
      }),
  ]);
  const classification = await classifyDiscordMessage("このゲームはテンポが悪いと思う", llm);
  assert.equal(llm.calls, 1);
  assert.equal(classification.action, "start_discussion");
  assert.equal(classification.title, "LLM分類議題");
  console.log("ok auto discussion can use LLM classification");
}

core.close();
console.log("auto-discussion.test.ts: all passed");
