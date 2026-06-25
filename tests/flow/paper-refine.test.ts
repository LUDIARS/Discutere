/**
 * ペーパー本文 LLM リファイン (paper-refine.ts) テスト。
 *  - 正常: LLM 出力 (見出し維持) をそのまま返す
 *  - degrade: LLM 失敗 / # 議題 欠落出力は null (= 呼び出し側は元の base を使う)
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";

const TMP = path.resolve(".tmp/flow-paper-refine");
fs.rmSync(TMP, { recursive: true, force: true });
fs.mkdirSync(TMP, { recursive: true });
process.env.DATABASE_PATH = path.join(TMP, "flow.db");

const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");
const { refinePaperBrief } = await import("../../src/flow/paper-refine.js");

_resetFlowDb();

const base = "# 議題\nガチャの是非\n\n# ゲームのメカニクス\n- **ガチャ**: 確率で入手";
const rounds = [{ round: 1, summary: "賛否が割れた", aufhebung: ["天井で救済"] }];

// ── 正常: LLM 出力をそのまま返す ──
{
  const refined = "# 議題\nガチャの是非 (天井前提)\n\n# 観点補足\n射幸性\n\n# ゲームのメカニクス\n- **天井**: 一定回数で確定";
  const llm = new MockLLMClient([() => refined]);
  const out = await refinePaperBrief({
    baseBodyMd: base,
    theme: "ガチャの是非",
    rounds,
    conclusion: "天井必須",
    llm,
    sessionId: "refine-ok",
  });
  assert.equal(out, refined, "正常系は LLM 出力をそのまま返す");
  // プロンプトに議論成果 (止揚/結論) が載る。
  assert.ok(llm.lastInvocation?.prompt.includes("天井で救済"), "プロンプトに止揚が含まれる");
  assert.ok(llm.lastInvocation?.prompt.includes("天井必須"), "プロンプトに結論が含まれる");
  console.log("  [ok] paper-refine: 正常系は改訂 md を返す + 成果をプロンプトに載せる");
}

// ── degrade: LLM 失敗は null ──
{
  const llm = new MockLLMClient([() => ({ ok: false as const, error: "boom" })]);
  const out = await refinePaperBrief({ baseBodyMd: base, theme: "t", rounds, llm, sessionId: "refine-err" });
  assert.equal(out, null, "LLM 失敗は null (degrade)");
  console.log("  [ok] paper-refine: LLM 失敗は null で degrade");
}

// ── degrade: # 議題 欠落出力は null ──
{
  const llm = new MockLLMClient([() => "# まとめ\n見出しが違う"]);
  const out = await refinePaperBrief({ baseBodyMd: base, theme: "t", rounds, llm, sessionId: "refine-bad" });
  assert.equal(out, null, "# 議題 欠落は null (degrade)");
  console.log("  [ok] paper-refine: 不正出力 (# 議題 欠落) は null で degrade");
}

delete process.env.DATABASE_PATH;
_resetFlowDb();

console.log("paper-refine tests: all passed");
