/**
 * adapter auto-export テスト (PR-E / MISSING #5-2).
 *
 * autoExportRootDir option を渡すと、 postUtterance / proposeHypothesis 後に
 * 該当 md ファイルが自動生成される。
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import { createCore } from "../../src/core/index.js";
import { createDiscatierContextProvider } from "../../src/discatier-engine-adapter/index.js";

const workDir = path.resolve(".tmp/pe-auto-export");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

const core = createCore(path.join(workDir, "db.kuzu"), path.join(workDir, "events.jsonl"));
const ws = "knowledge";
const rootDir = path.join(workDir, "discussions");

try {
  const sessionId = core.repos.session.create({
    workspaceId: ws,
    title: "auto-export session",
    startedAt: Date.now(),
  });

  const adapter = createDiscatierContextProvider(core, {
    defaultSessionId: sessionId,
    autoExportRootDir: rootDir,
  });

  // 1. proposeHypothesis 後に hypothesis md が生成
  const propose = adapter.proposeHypothesis({
    workspaceId: ws,
    statement: "auto-exported hypothesis",
    byPersonaId: "advocate",
  });
  // dump は dynamic import なので少し待つ
  await waitFor(
    () =>
      fs.existsSync(
        path.join(rootDir, ws, "hypothesis", `${propose.id}.md`)
      ),
    500
  );
  const hypMd = fs.readFileSync(
    path.join(rootDir, ws, "hypothesis", `${propose.id}.md`),
    "utf8"
  );
  assert.ok(hypMd.includes("auto-exported hypothesis"));
  console.log("ok auto-export hypothesis md created");

  // 2. postUtterance 後に utterance md が生成
  const utt = adapter.postUtterance({
    workspaceId: ws,
    sessionId,
    text: "auto-exported utterance",
    byPersonaId: "sceptic",
  });
  await waitFor(
    () =>
      fs.existsSync(
        path.join(rootDir, ws, "utterance", `${utt.id}.md`)
      ),
    500
  );
  const uttMd = fs.readFileSync(
    path.join(rootDir, ws, "utterance", `${utt.id}.md`),
    "utf8"
  );
  assert.ok(uttMd.includes("auto-exported utterance"));
  console.log("ok auto-export utterance md created");

  // 3. autoExportRootDir 未指定なら md 生成しない
  const adapter2 = createDiscatierContextProvider(core, {
    defaultSessionId: sessionId,
  });
  const before = listMdFiles(rootDir);
  adapter2.proposeHypothesis({
    workspaceId: ws,
    statement: "no auto-export",
    byPersonaId: "refiner",
  });
  await wait(150);
  const after = listMdFiles(rootDir);
  assert.equal(
    after.length,
    before.length,
    "no autoExportRootDir → no new md files"
  );
  console.log("ok no autoExport when option omitted");
} finally {
  core.close();
}

console.log("auto-export.test.ts: all passed");

async function waitFor(check: () => boolean, timeoutMs: number): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (check()) return;
    await wait(20);
  }
  assert.ok(check(), "timeout waiting for condition");
}

function wait(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}

function listMdFiles(dir: string): string[] {
  if (!fs.existsSync(dir)) return [];
  const out: string[] = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true, recursive: true }) as fs.Dirent[]) {
    if (entry.isFile() && entry.name.endsWith(".md")) {
      out.push(entry.name);
    }
  }
  return out;
}
