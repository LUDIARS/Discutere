import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import matter from "gray-matter";

import { createCore } from "../../src/core/index.js";
import {
  exportGap,
  exportHypothesis,
  exportMechanic,
  exportSession,
  exportUtterance,
  ExporterError,
} from "../../src/visualize/index.js";

const workDir = path.resolve(".tmp/visualize-md-exporter");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });
const core = createCore(path.join(workDir, "db.kuzu"), path.join(workDir, "events.jsonl"));
const ws = "knowledge";

try {
  // 1. Game + Mechanic + Aesthetic を用意
  const gameId = core.repos.game.create({ workspaceId: ws, title: "Test Game", genre: "RPG" });
  const mechId = core.repos.mechanic.create({
    workspaceId: ws,
    gameId,
    name: "Cast Spell",
    description: "魔法を撃つ",
  });
  // intends / intended_affect は events 経由ではなく raw insert で揃える必要あり
  core.client.raw
    .prepare(
      "INSERT OR IGNORE INTO mechanics (id, workspace_id, game_id, name, description, intends, intended_affect, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
    )
    .run(
      mechId,
      ws,
      gameId,
      "Cast Spell",
      "魔法を撃つ",
      "リソース管理の戦術判断",
      "tension",
      Date.now(),
      Date.now()
    );
  core.client.raw
    .prepare("UPDATE mechanics SET intends = ?, intended_affect = ? WHERE id = ?")
    .run("リソース管理の戦術判断", "tension", mechId);

  const aesId = core.repos.aesthetic.create({
    workspaceId: ws,
    gameId,
    name: "中世ファンタジー",
    description: "石造城",
  });

  // 2. Design Gap
  const gapId = core.repos.designGap.create({
    workspaceId: ws,
    gameId,
    title: "魔法の学習曲線",
    description: "新規プレイヤーが MP 概念で躓く",
    status: "open",
  });

  // 3. Hypothesis (gap に紐付け + refs に utterance id)
  const sessionId = core.repos.session.create({
    workspaceId: ws,
    title: "design discussion",
    startedAt: Date.now(),
  });
  const uttId = core.repos.utterance.create({
    workspaceId: ws,
    sessionId,
    rawContent: "MP の意味がわからない",
    postedAt: Date.now(),
    speakerId: "alice",
  });
  const hypId = core.repos.hypothesis.create({
    workspaceId: ws,
    designGapId: gapId,
    statement: "リソース管理の戦術判断",
    status: "validated",
  });
  // refs_json と validated_by_emotion は phase 2/5 で event 経由になるが、
  // テストでは raw update で十分
  core.client.raw
    .prepare(
      "UPDATE hypotheses SET refs_json = ?, validated_by_emotion = 1 WHERE id = ?"
    )
    .run(JSON.stringify([uttId]), hypId);

  // ── hypothesis ──
  {
    const r = exportHypothesis(core, ws, hypId);
    const parsed = matter(r.markdown);
    assert.equal(parsed.data.id, hypId);
    assert.equal(parsed.data.type, "hypothesis");
    assert.equal(parsed.data.status, "validated");
    assert.equal(parsed.data.addresses, `[[gap:${gapId}]]`);
    assert.ok(Array.isArray(parsed.data.refs));
    assert.equal(parsed.data.refs[0], `[[utt:${uttId}]]`);
    assert.ok(parsed.data.validated_by.includes("emotion"));
    // outgoing には gap + utt + 派生 mch が含まれる
    const outTypes = r.outgoing.map((l) => l.type).sort();
    assert.ok(outTypes.includes("gap"));
    assert.ok(outTypes.includes("utt"));
    assert.ok(outTypes.includes("mch")); // intends LIKE 逆引きで Cast Spell 拾う
    console.log("ok exportHypothesis");
  }

  // ── gap ──
  {
    const r = exportGap(core, ws, gapId);
    const parsed = matter(r.markdown);
    assert.equal(parsed.data.id, gapId);
    assert.equal(parsed.data.type, "gap");
    assert.equal(parsed.data.title, "魔法の学習曲線");
    assert.ok(Array.isArray(parsed.data.hypotheses));
    assert.equal(parsed.data.hypotheses[0], `[[hyp:${hypId}]]`);
    console.log("ok exportGap");
  }

  // ── mechanic ──
  {
    const r = exportMechanic(core, ws, mechId);
    const parsed = matter(r.markdown);
    assert.equal(parsed.data.id, mechId);
    assert.equal(parsed.data.type, "mechanic");
    assert.equal(parsed.data.intends, "リソース管理の戦術判断");
    assert.equal(parsed.data.intended_affect, "tension");
    assert.equal(parsed.data.game, `[[game:${gameId}]]`);
    console.log("ok exportMechanic");
  }

  // ── session + utterance ──
  {
    const sr = exportSession(core, ws, sessionId);
    const sParsed = matter(sr.markdown);
    assert.equal(sParsed.data.title, "design discussion");
    assert.ok(Array.isArray(sParsed.data.utterances));
    assert.equal(sParsed.data.utterances[0], `[[utt:${uttId}]]`);

    const ur = exportUtterance(core, ws, uttId);
    const uParsed = matter(ur.markdown);
    assert.equal(uParsed.data.session, `[[ses:${sessionId}]]`);
    assert.equal(uParsed.data.speaker, "alice");
    assert.ok(ur.markdown.includes("> MP の意味がわからない"));
    assert.ok(Array.isArray(uParsed.data.referenced_by));
    assert.equal(uParsed.data.referenced_by[0], `[[hyp:${hypId}]]`);
    console.log("ok exportSession + Utterance");
  }

  // 不存在 id → ExporterError
  assert.throws(
    () => exportHypothesis(core, ws, "missing-id"),
    ExporterError
  );
  console.log("ok ExporterError on missing id");

  // aesthetic 動作確認 (簡易)
  {
    const aesthetics = core.client.raw
      .prepare("SELECT id FROM aesthetics WHERE id = ?")
      .all(aesId);
    assert.equal(aesthetics.length, 1);
  }
} finally {
  core.close();
}

console.log("md-exporter.test.ts: all passed");
