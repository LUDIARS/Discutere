/**
 * dump.ts のテスト: 「議論ソースを md ファイルに書き出して読み戻せる」 を end-to-end で検証。
 * GraphDB → md ファイル → wikilink で参照可能、 のラウンドトリップ。
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import matter from "gray-matter";

import { createCore } from "../../src/core/index.js";
import {
  dumpNode,
  extractWikilinks,
  resolveFilePath,
} from "../../src/visualize/index.js";

const workDir = path.resolve(".tmp/visualize-dump");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });
const rootDir = path.join(workDir, "discussions");

const core = createCore(path.join(workDir, "db.kuzu"), path.join(workDir, "events.jsonl"));
const ws = "team-alpha";

try {
  const gameId = core.repos.game.create({ workspaceId: ws, title: "DG", genre: "Adv" });
  const gapId = core.repos.designGap.create({
    workspaceId: ws,
    gameId,
    title: "Onboarding drop",
    status: "open",
  });
  const sid = core.repos.session.create({
    workspaceId: ws,
    title: "design session",
    startedAt: Date.now(),
  });
  const uttId = core.repos.utterance.create({
    workspaceId: ws,
    sessionId: sid,
    rawContent: "選択肢が多いと迷う",
    postedAt: Date.now(),
    speakerId: "bob",
  });
  const hypId = core.repos.hypothesis.create({
    workspaceId: ws,
    designGapId: gapId,
    statement: "選択肢を絞ると離脱が減る",
    status: "validated",
  });
  core.client.raw
    .prepare("UPDATE hypotheses SET refs_json = ? WHERE id = ?")
    .run(JSON.stringify([uttId]), hypId);

  // 1. hypothesis を dump → file が作られる
  const r1 = dumpNode(core, "hyp", hypId, { workspaceId: ws, rootDir });
  assert.equal(r1.written, "created");
  assert.ok(r1.filePath.endsWith(`${path.sep}hypothesis${path.sep}${hypId}.md`));
  assert.ok(fs.existsSync(r1.filePath));
  console.log("ok dump created");

  // 2. ファイル中身を読み戻して frontmatter / wikilink を確認
  const written = fs.readFileSync(r1.filePath, "utf8");
  const parsed = matter(written);
  assert.equal(parsed.data.id, hypId);
  assert.equal(parsed.data.workspace_id, ws);
  assert.equal(parsed.data.addresses, `[[gap:${gapId}]]`);
  const allLinks = extractWikilinks(written);
  const linkTypes = new Set(allLinks.map((l) => l.type));
  assert.ok(linkTypes.has("gap"));
  assert.ok(linkTypes.has("utt"));
  console.log("ok md frontmatter + wikilinks readable");

  // 3. resolveFilePath が予測可能
  const expected = resolveFilePath("hyp", hypId, { workspaceId: ws, rootDir });
  assert.equal(expected, r1.filePath);
  console.log("ok resolveFilePath");

  // 4. 同じ内容で再 dump → unchanged
  const r2 = dumpNode(core, "hyp", hypId, { workspaceId: ws, rootDir });
  assert.equal(r2.written, "unchanged");
  console.log("ok idempotent unchanged");

  // 5. データを変えて再 dump → updated
  core.client.raw
    .prepare("UPDATE hypotheses SET statement = ?, updated_at = ? WHERE id = ?")
    .run("選択肢の量より配列順", Date.now() + 1000, hypId);
  const r3 = dumpNode(core, "hyp", hypId, { workspaceId: ws, rootDir });
  assert.equal(r3.written, "updated");
  const reread = fs.readFileSync(r3.filePath, "utf8");
  assert.ok(reread.includes("選択肢の量より配列順"));
  console.log("ok updated on change");

  // 6. gap も dump して、 hypothesis から gap への link が辿れる
  const r4 = dumpNode(core, "gap", gapId, { workspaceId: ws, rootDir });
  assert.equal(r4.written, "created");
  const gapMd = fs.readFileSync(r4.filePath, "utf8");
  const gapLinks = extractWikilinks(gapMd);
  assert.ok(
    gapLinks.some((l) => l.type === "hyp" && l.id === hypId),
    "gap md should link back to hypothesis"
  );

  // hypothesis md から gap md への path が解決できることを確認 (= マジックリンクが意味を持つ)
  const hypMd = fs.readFileSync(r1.filePath, "utf8");
  const hypGapLink = extractWikilinks(hypMd).find((l) => l.type === "gap");
  assert.ok(hypGapLink);
  const resolvedGapPath = resolveFilePath(hypGapLink.type, hypGapLink.id, {
    workspaceId: ws,
    rootDir,
  });
  assert.equal(resolvedGapPath, r4.filePath);
  console.log("ok cross-link resolution (hyp → gap md path)");
} finally {
  core.close();
}

console.log("dump.test.ts: all passed");
