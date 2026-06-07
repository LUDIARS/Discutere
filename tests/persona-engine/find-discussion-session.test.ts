import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import { createCore } from "../../src/core/index.js";
import { createDiscatierContextProvider } from "../../src/discatier-engine-adapter/index.js";

const workDir = path.resolve(".tmp/find-discussion-session");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

const ws = "knowledge";
const core = createCore(path.join(workDir, "db.kuzu"), path.join(workDir, "events.jsonl"));
const adapter = createDiscatierContextProvider(core);

const gapId = core.repos.designGap.create({ workspaceId: ws, title: "面白さの本質" });
const sessionId = core.repos.session.create({
  workspaceId: ws,
  title: `discussion-of-gap:${gapId}`,
  startedAt: Date.now(),
});
const hid = core.repos.hypothesis.create({ workspaceId: ws, designGapId: gapId, statement: "自動攻撃が手軽さを生む" });

// gapId から解決
assert.equal(adapter.findDiscussionSession?.({ workspaceId: ws, gapId }), sessionId, "gapId → discussion session");
// hypothesisId から (→ design_gap_id → session) 解決
assert.equal(adapter.findDiscussionSession?.({ workspaceId: ws, hypothesisId: hid }), sessionId, "hypothesisId → discussion session");
// 未知は null
assert.equal(adapter.findDiscussionSession?.({ workspaceId: ws, hypothesisId: "nope" }), null, "未知 hypothesis は null");
assert.equal(adapter.findDiscussionSession?.({ workspaceId: ws, gapId: "nope" }), null, "未知 gap は null");

// ended_at が入った session (= 管理画面から「閉じた」議論) は gap が open でも null。
core.client.raw
  .prepare("UPDATE sessions SET ended_at = ? WHERE id = ?")
  .run(Date.now(), sessionId);
assert.equal(
  adapter.findDiscussionSession?.({ workspaceId: ws, gapId }),
  null,
  "ended session は着地対象にしない"
);

core.close();
console.log("ok findDiscussionSession (tick rule の session 解決)");
