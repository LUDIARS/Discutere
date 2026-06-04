import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

import { createCore } from "../../src/core/index.js";
import { createDiscatierContextProvider } from "../../src/discatier-engine-adapter/index.js";
import { createEventBridge } from "../../src/discatier-engine-adapter/event-bridge.js";
import {
  applyPersonaEngineMigrations,
  createPersonaEngine,
  DISCUSSION_PERSONA_SEEDS,
  DISCUSSION_RULE_SEEDS,
  MockLLMClient,
  type Logger,
} from "../../src/persona-engine/index.js";

const workDir = path.resolve(".tmp/pe-event-bridge-discord");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

const core = createCore(path.join(workDir, "db.kuzu"), path.join(workDir, "events.jsonl"));
const peDb = new Database(path.join(workDir, "pe.db"));
applyPersonaEngineMigrations(peDb);

const ws = "knowledge";
const guildId = "guild-1";
const channelId = "channel-1";
const sourceSessionId = core.repos.session.create({
  workspaceId: ws,
  title: `discord-session:${guildId}:${channelId}`,
  startedAt: Date.now(),
  scene: `discord:${guildId}/${channelId}`,
});
const utteranceId = core.repos.utterance.create({
  workspaceId: ws,
  sessionId: sourceSessionId,
  speakerId: "u1",
  rawContent: "ヴァンサバの面白いところどこだろう",
  postedAt: Date.now(),
});

const llm = new MockLLMClient([
  () =>
    JSON.stringify({
      action: "propose_hypothesis",
      statement: "discord proposal",
      reasoning: "r",
    }),
]);

const logger: Logger = {
  debug: () => {},
  info: () => {},
  warn: () => {},
  error: () => {},
};

let postedDesignGapId: string | null = null;
const adapter = createDiscatierContextProvider(core, {
  onPostedHypothesis(input) {
    postedDesignGapId = input.designGapId;
  },
});

const engine = createPersonaEngine({
  db: peDb,
  llm,
  contextProvider: adapter,
  logger,
  workspaceId: ws,
  personaSeeds: DISCUSSION_PERSONA_SEEDS,
  ruleSeeds: DISCUSSION_RULE_SEEDS,
  tickMs: 60_000,
  enableLlm: true,
});
peDb.prepare("UPDATE rules SET cooldown_sec = 0").run();

const bridge = createEventBridge(core, engine, { workspaceId: ws, pollMs: 60_000 });

try {
  const gapId = core.repos.designGap.create({
    workspaceId: ws,
    title: "ヴァンサバの面白さの本質",
    description: "Discord投稿から自動検出",
    status: "open",
  });
  core.client.raw
    .prepare("UPDATE design_gaps SET evidence_json = ?, updated_at = ? WHERE id = ?")
    .run(
      JSON.stringify({
        source: "discord:auto-classifier",
        category: "game_design_question",
        utteranceIds: [utteranceId],
        guildId,
        channelId,
        sessionId: sourceSessionId,
        authorId: "u1",
      }),
      Date.now(),
      gapId
    );

  const dispatched = await bridge.pollOnce();
  assert.ok(dispatched >= 1);

  const hyp = core.client.raw
    .prepare("SELECT statement, design_gap_id FROM hypotheses WHERE workspace_id = ? ORDER BY created_at DESC LIMIT 1")
    .get(ws) as { statement: string; design_gap_id: string | null };
  assert.equal(hyp.statement, "discord proposal");
  assert.equal(hyp.design_gap_id, gapId);
  assert.equal(postedDesignGapId, gapId);

  const gapSession = core.client.raw
    .prepare("SELECT scene FROM sessions WHERE workspace_id = ? AND title = ?")
    .get(ws, `discussion-of-gap:${gapId}`) as { scene: string | null };
  assert.equal(gapSession.scene, `discord:${guildId}/${channelId}`);

  console.log("ok discord gap session and hypothesis inherit source gap");
} finally {
  bridge.stop();
  engine.stop();
  peDb.close();
  core.close();
}

console.log("event-bridge-discord.test.ts: all passed");
