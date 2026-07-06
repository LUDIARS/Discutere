/**
 * command-router (transport 非依存) テスト.
 *
 * WS Gateway / HTTP の双方から呼ばれる slash command ルーティングのうち、
 * persona-engine 制御 (discutere-kill / discutere-status) と admin allowlist を検証する。
 * Discatier command (define/propose 等) の経路は core DB を要するため別 e2e で扱う。
 */

import assert from "node:assert/strict";

import {
  routeInboundMessage,
  routeSlashCommand,
  type CommandRouterDeps,
  type InboundSlashCommand,
} from "../../src/discord-hook/command-router.js";
import type { DiscordInboundMessage } from "../../src/discord-hook/types.js";
import type { PersonaEngineHandle } from "../../src/persona-engine/index.js";

// ─── fake engine ────────────────────────────────
let lastSetEnabled: boolean | null = null;
const fakeEngine = {
  setRulesEnabled: (v: boolean) => {
    lastSetEnabled = v;
  },
  personas: { list: () => [{ id: "p1" }, { id: "p2" }] },
  rules: { list: (_f?: unknown) => [{ id: "r1" }] },
} as unknown as PersonaEngineHandle;

function depsWith(adminIds: string[], engine: PersonaEngineHandle | null): CommandRouterDeps {
  return { workspaceId: "knowledge", adminIds, discussionChannelIds: [], getEngine: () => engine };
}

function slash(partial: Partial<InboundSlashCommand>): InboundSlashCommand {
  return {
    name: "discutere-status",
    argsText: "",
    userId: "admin1",
    guildId: "g1",
    channelId: "ch1",
    ...partial,
  };
}

// 1. adminIds 未設定 → 全 deny (安全 default)
{
  const r = routeSlashCommand(slash({ name: "discutere-status" }), depsWith([], fakeEngine));
  assert.ok(r.ephemeral);
  assert.ok(r.content.includes("disabled"), `expected disabled msg, got: ${r.content}`);
  console.log("ok empty allowlist denies admin slash");
}

// 2. 非 admin user → reject
{
  const r = routeSlashCommand(
    slash({ name: "discutere-status", userId: "intruder" }),
    depsWith(["admin1"], fakeEngine)
  );
  assert.ok(r.content.includes("admin only"), `got: ${r.content}`);
  console.log("ok non-admin rejected");
}

// 3. admin + status → persona/rule 件数
{
  const r = routeSlashCommand(
    slash({ name: "discutere-status", userId: "admin1" }),
    depsWith(["admin1"], fakeEngine)
  );
  assert.ok(r.content.includes("personas: `2`"), `got: ${r.content}`);
  assert.ok(r.content.includes("active rules: `1`"), `got: ${r.content}`);
  console.log("ok status reports counts");
}

// 4. admin + kill enabled=false → engine.setRulesEnabled(false)
{
  lastSetEnabled = null;
  const r = routeSlashCommand(
    slash({ name: "discutere-kill", userId: "admin1", enabledOption: false }),
    depsWith(["admin1"], fakeEngine)
  );
  assert.equal(lastSetEnabled, false, "engine disabled");
  assert.ok(r.content.includes("DISABLED"), `got: ${r.content}`);
  console.log("ok kill disables engine");
}

// 5. admin + kill enabled=true → enable
{
  lastSetEnabled = null;
  const r = routeSlashCommand(
    slash({ name: "discutere-kill", userId: "admin1", enabledOption: true }),
    depsWith(["admin1"], fakeEngine)
  );
  assert.equal(lastSetEnabled, true, "engine enabled");
  assert.ok(r.content.includes("ENABLED"), `got: ${r.content}`);
  console.log("ok kill enables engine");
}

// 6. kill without enabled option → usage
{
  const r = routeSlashCommand(
    slash({ name: "discutere-kill", userId: "admin1", enabledOption: undefined }),
    depsWith(["admin1"], fakeEngine)
  );
  assert.ok(r.content.includes("usage:"), `got: ${r.content}`);
  console.log("ok kill without option shows usage");
}

// 7. engine 未初期化 → not initialized
{
  const r = routeSlashCommand(
    slash({ name: "discutere-status", userId: "admin1" }),
    depsWith(["admin1"], null)
  );
  assert.ok(r.content.includes("not initialized"), `got: ${r.content}`);
  console.log("ok engine-not-initialized handled");
}

// 8. routeInboundMessage は非許可チャンネルを早期 return (core に触れず副作用なし)
{
  const msg: DiscordInboundMessage = {
    id: "m1",
    channelId: "ch-not-allowed",
    author: { id: "u1", username: "alice" },
    content: "ふつうの発言",
    mentions: [],
    timestamp: new Date(0).toISOString(),
  };
  // discussionChannelIds が空 / 不一致なら createCore に到達せず throw しない。
  // 非取込なので ingested=false かつ seed=undefined (caller はリアクションを付けない)。
  const r1 = routeInboundMessage(msg, "g1", depsWith(["admin1"], fakeEngine));
  assert.equal(r1.ingested, false);
  assert.equal(r1.seed, undefined);
  const r2 = routeInboundMessage(
    msg,
    "g1",
    { workspaceId: "knowledge", adminIds: [], discussionChannelIds: ["ch-other"], getEngine: () => null }
  );
  assert.equal(r2.ingested, false);
  assert.equal(r2.seed, undefined);
  console.log("ok inbound message gated by discussionChannelIds (non-allowed channel ignored)");
}

// 9. /discutere-discussions — 議論一覧 (state 絞り込み) を ephemeral で返す
{
  const path = await import("node:path");
  const fs = await import("node:fs");
  const TMP = path.resolve(".tmp/cmd-router-discussions");
  fs.rmSync(TMP, { recursive: true, force: true });
  fs.mkdirSync(TMP, { recursive: true });
  process.env.DATABASE_PATH = path.join(TMP, "flow.db");

  const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
  const { persistPaper, persistDraftPaper } = await import("../../src/flow/discussion-paper.js");
  _resetFlowDb();
  persistPaper({ sessionId: "d-live-1", theme: "進行中の議題", tags: [], mechanics: [], supplement: "", bodyMd: "# 議題\n進行中の議題" }, "discussion");
  persistDraftPaper({ sessionId: "d-draft-1", theme: "下書きの議題", tags: [], mechanics: [], supplement: "", bodyMd: "# 議題\n下書きの議題" }, "discussion");

  const deps = depsWith([], null);
  const rAll = routeSlashCommand(slash({ name: "discutere-discussions", argsText: "" }), deps);
  assert.ok(rAll.ephemeral, "ephemeral で返す");
  assert.ok(rAll.content.includes("進行中の議題"), "全件に live が出る");
  assert.ok(rAll.content.includes("下書きの議題"), "全件に draft が出る");
  assert.match(rAll.content, /#\d+ \*\*進行中の議題\*\*/, "議論番号を表示する");

  const rDraft = routeSlashCommand(slash({ name: "discutere-discussions", argsText: "draft" }), deps);
  assert.ok(rDraft.content.includes("下書きの議題"), "draft 絞り込みに draft");
  assert.ok(!rDraft.content.includes("進行中の議題"), "draft 絞り込みに live は出ない");
  assert.ok(rDraft.content.includes("📝下書き"), "下書きバッジが付く");

  _resetFlowDb();
  delete process.env.DATABASE_PATH;
  console.log("ok /discutere-discussions lists + filters by state");
}

console.log("command-router.test.ts: all passed");
