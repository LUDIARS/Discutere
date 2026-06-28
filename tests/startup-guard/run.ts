/**
 * production 起動 health guard (src/startup-guard.ts) の単体テスト。
 * guard が読むのは nodeEnv / discord.botToken / discord.adminIds / llm.backend のみ
 * なので、最小オブジェクトを DiscutereConfig として渡す。
 */

import assert from "node:assert/strict";

import { assertStartupHealth, StartupHealthError } from "../../src/startup-guard.js";
import type { DiscutereConfig } from "../../src/config.js";

function makeConfig(o: {
  nodeEnv: string;
  botToken?: string;
  adminIds?: string[];
  backend?: string;
}): DiscutereConfig {
  return {
    nodeEnv: o.nodeEnv,
    discord: { botToken: o.botToken, adminIds: o.adminIds ?? [] },
    llm: { backend: o.backend ?? "claude-cli" },
  } as unknown as DiscutereConfig;
}

function captureWarn(): { logger: Pick<Console, "warn">; warns: string[] } {
  const warns: string[] = [];
  return { logger: { warn: (...a: unknown[]) => warns.push(a.join(" ")) }, warns };
}

// dev は no-op (botToken 空でも throw しない)
assertStartupHealth(makeConfig({ nodeEnv: "development", botToken: "" }));
console.log("ok dev no-op");

// production + botToken 空 → 致命 (StartupHealthError)
assert.throws(
  () => assertStartupHealth(makeConfig({ nodeEnv: "production", botToken: "" })),
  StartupHealthError,
  "production + botToken 空は致命"
);
console.log("ok prod missing botToken throws");

// production + botToken あり + adminIds 空 → warn 1 件・throw しない
{
  const { logger, warns } = captureWarn();
  assertStartupHealth(makeConfig({ nodeEnv: "production", botToken: "t", adminIds: [] }), logger);
  assert.ok(warns.some((w) => w.includes("adminIds")), "adminIds 空は warn");
}
console.log("ok prod empty adminIds warns");

// production + mock backend → warn
{
  const { logger, warns } = captureWarn();
  assertStartupHealth(
    makeConfig({ nodeEnv: "production", botToken: "t", adminIds: ["a"], backend: "mock" }),
    logger
  );
  assert.ok(warns.some((w) => w.includes("mock")), "mock backend は warn");
}
console.log("ok prod mock backend warns");

// production すべて健全 → throw なし・warn なし
{
  const { logger, warns } = captureWarn();
  assertStartupHealth(
    makeConfig({ nodeEnv: "production", botToken: "t", adminIds: ["a"], backend: "claude-cli" }),
    logger
  );
  assert.equal(warns.length, 0, "健全な production は warn 無し");
}
console.log("ok prod healthy no warn");

console.log("startup-guard: all passed");
