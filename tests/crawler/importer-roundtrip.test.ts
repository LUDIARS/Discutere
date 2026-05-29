import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import { createCore } from "../../src/core/index.js";
import { importGameKG, parseGameKG } from "../../src/crawler/index.js";

const workDir = path.resolve(".tmp/crawler-roundtrip");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

const SAMPLE = `---
id: rt-test
title: "RT Test Game"
genre: "アクション"
workspace_id: "knowledge"
sources:
  - url: "https://example.com/rt"
    excerpt_policy: "summary-only"
mechanics:
  - name: "Jump"
    description: "上方向に跳ぶ"
    intends: "縦軸機動の起点"
    intended_affect: "joy"
  - name: "Slide"
    description: "低姿勢移動"
aesthetics:
  - name: "ピクセル風"
    description: "16-bit 解像度の意図的レトロ"
---
`;

const core = createCore(path.join(workDir, "db.kuzu"), path.join(workDir, "events.jsonl"));
try {
  const kg = parseGameKG(SAMPLE);

  // 1. 初回 import → insert
  const r1 = importGameKG(core, kg);
  assert.ok(r1.gameId);
  assert.equal(r1.inserted.mechanics, 2);
  assert.equal(r1.inserted.aesthetics, 1);
  assert.equal(r1.updated.mechanics, 0);
  assert.equal(r1.updated.aesthetics, 0);
  assert.equal(r1.updated.game, false);
  console.log("ok initial insert");

  // 2. 再 import (idempotent) → update path、 同じ gameId
  const r2 = importGameKG(core, kg);
  assert.equal(r2.gameId, r1.gameId);
  assert.equal(r2.inserted.mechanics, 0);
  assert.equal(r2.updated.mechanics, 2);
  console.log("ok idempotent re-import");

  // 3. mechanic 1 件追加した md を再 import → 1 つだけ insert される
  const grown = SAMPLE.replace(
    "  - name: \"Slide\"\n    description: \"低姿勢移動\"",
    "  - name: \"Slide\"\n    description: \"低姿勢移動\"\n  - name: \"Wall Jump\"\n    description: \"壁から再ジャンプ\""
  );
  const kg2 = parseGameKG(grown);
  const r3 = importGameKG(core, kg2);
  assert.equal(r3.gameId, r1.gameId);
  assert.equal(r3.inserted.mechanics, 1);
  assert.equal(r3.updated.mechanics, 2);
  console.log("ok partial growth");
} finally {
  core.close();
}

console.log("importer-roundtrip.test.ts: all passed");
