/**
 * Claude Code OAuth トークンリーダ (E: SDK をサブスクで使う) のテスト。
 * ファイル読込のみ (DB 非依存)。valid / expired / 秒epoch / 不在 / 壊れ。
 */

import assert from "node:assert/strict";
import { writeFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";

const { readClaudeCodeToken, _resetClaudeCodeTokenCache } = await import(
  "../../src/persona-engine/llm/claude-code-auth.js"
);

const dir = mkdtempSync(path.join(tmpdir(), "cc-auth-"));
const f = (name: string, obj: unknown): string => {
  const p = path.join(dir, name);
  writeFileSync(p, JSON.stringify(obj));
  return p;
};

_resetClaudeCodeTokenCache();
assert.equal(
  readClaudeCodeToken(f("a.json", { claudeAiOauth: { accessToken: "tok-A", expiresAt: Date.now() + 3600_000 } })),
  "tok-A",
  "valid (ms 未来)"
);

_resetClaudeCodeTokenCache();
assert.equal(
  readClaudeCodeToken(f("b.json", { claudeAiOauth: { accessToken: "tok-B", expiresAt: Date.now() - 1000 } })),
  null,
  "expired"
);

_resetClaudeCodeTokenCache();
assert.equal(
  readClaudeCodeToken(f("c.json", { claudeAiOauth: { accessToken: "tok-C", expiresAt: Math.floor(Date.now() / 1000) + 3600 } })),
  "tok-C",
  "秒 epoch 未来"
);

_resetClaudeCodeTokenCache();
assert.equal(readClaudeCodeToken(path.join(dir, "nope.json")), null, "不在");

_resetClaudeCodeTokenCache();
assert.equal(readClaudeCodeToken(f("d.json", { foo: 1 })), null, "壊れ/鍵なし");

console.log("  [ok] claude-code-auth: valid/expired/秒epoch/不在/壊れ");
console.log("claude-code-auth tests: all passed");
