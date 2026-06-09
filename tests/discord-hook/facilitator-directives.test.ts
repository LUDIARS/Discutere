import fs from "node:fs";
import path from "node:path";
import assert from "node:assert/strict";

import { createCore } from "../../src/core/index.js";
import {
  addFacilitatorDirective,
  ensureFacilitatorDirectiveTable,
  formatDirectivesBlock,
  listFacilitatorDirectives,
  normalizeDirectiveText,
  resolveActiveGapForScene,
  stripBotMention,
  DIRECTIVE_TEXT_CAP,
} from "../../src/discord-hook/facilitator-directives.js";

const dir = path.resolve(".tmp/discord-facilitator-directives");
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });

const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
const raw = core.client.raw;
const workspaceId = "knowledge";

// ── pure helpers ──
{
  assert.equal(stripBotMention("<@123> もっと簡単に", "123"), "もっと簡単に");
  assert.equal(stripBotMention("<@!123>  否定的に  ", "123"), "否定的に");
  assert.equal(stripBotMention("メンション無し", "123"), "メンション無し");
  assert.equal(normalizeDirectiveText("   "), null);
  assert.equal(normalizeDirectiveText("x".repeat(DIRECTIVE_TEXT_CAP + 50))!.length, DIRECTIVE_TEXT_CAP);
  assert.equal(formatDirectivesBlock([]), "");
  assert.ok(formatDirectivesBlock(["簡単に"]).includes("簡単に"));
  console.log("ok facilitator-directives pure helpers");
}

// ── store + list (時系列順) ──
{
  ensureFacilitatorDirectiveTable(raw);
  const gapId = "gap-x";
  addFacilitatorDirective(raw, { workspaceId, gapId, text: "もっと簡単な言葉で", authorId: "u1" });
  addFacilitatorDirective(raw, { workspaceId, gapId, text: "もっと否定的に", authorId: "u2" });
  // 空文は保存しない。
  assert.equal(addFacilitatorDirective(raw, { workspaceId, gapId, text: "   " }), null);
  const list = listFacilitatorDirectives(raw, workspaceId, gapId);
  assert.deepEqual(list, ["もっと簡単な言葉で", "もっと否定的に"]);
  // 別 gap は混ざらない。
  assert.deepEqual(listFacilitatorDirectives(raw, workspaceId, "gap-other"), []);
  console.log("ok facilitator-directives store + chronological list");
}

// ── resolveActiveGapForScene: open な discussion-of-gap session を scene で引く ──
{
  const gapId = core.repos.designGap.create({
    workspaceId,
    title: "対象ゲームの議題",
    description: "anchor",
    status: "open",
  });
  core.repos.session.create({
    workspaceId,
    title: `discussion-of-gap:${gapId}`,
    startedAt: Date.now(),
    scene: "discord:g1/ch1",
  } as never);
  assert.equal(resolveActiveGapForScene(raw, workspaceId, "discord:g1/ch1"), gapId);
  // 無関係 scene は null。
  assert.equal(resolveActiveGapForScene(raw, workspaceId, "discord:g9/ch9"), null);

  // closed gap は対象外。
  core.repos.designGap.update(gapId, { workspaceId, title: "対象ゲームの議題", description: "anchor", status: "closed" });
  assert.equal(resolveActiveGapForScene(raw, workspaceId, "discord:g1/ch1"), null);
  console.log("ok resolveActiveGapForScene (open only)");
}

core.close();
console.log("facilitator-directives tests: all passed");
