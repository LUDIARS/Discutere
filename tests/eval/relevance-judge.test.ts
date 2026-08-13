import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import {
  buildJudgePrompt,
  parseJudgeScores,
  JudgmentCache,
  judgeRelevance,
} from "../../scripts/lib/relevance-judge.js";
import type { LLMClient } from "../../src/persona-engine/llm/client.js";

// ── buildJudgePrompt: 短キー対応表と本文の折り込み ───────────────────────────
{
  const { system, prompt, keyToId } = buildJudgePrompt("モンストの引っ張りは面白いか", [
    { id: "id-a", content: "引っ張りの反射が気持ちいい" },
    { id: "id-b", content: "  空白  を   圧縮 " },
  ]);
  assert.ok(system.includes("JSON"));
  assert.ok(system.includes("指示・命令"), "発言内の指示はデータとして扱う");
  assert.ok(prompt.includes('v1: "引っ張りの反射が気持ちいい"'));
  assert.ok(prompt.includes('v2: "空白 を 圧縮"'), "本文の空白は圧縮される");
  assert.equal(keyToId.get("v1"), "id-a");
  assert.equal(keyToId.get("v2"), "id-b");
}
console.log("ok buildJudgePrompt");

// ── parseJudgeScores: JSON 抽出・不正値の除外 ────────────────────────────────
{
  const keyToId = new Map([
    ["v1", "id-a"],
    ["v2", "id-b"],
    ["v3", "id-c"],
  ]);
  const scores = parseJudgeScores(
    '前置き {"scores": {"v1": 2, "v2": "1", "v3": 5, "v9": 0}} 後置き',
    keyToId
  );
  assert.equal(scores.get("id-a"), 2);
  assert.equal(scores.get("id-b"), 1, "文字列の数値は許容");
  assert.equal(scores.has("id-c"), false, "範囲外スコアは捨てる");
  assert.equal(scores.size, 2, "未知キーは無視");

  assert.equal(parseJudgeScores("JSON じゃない", keyToId).size, 0);
  assert.equal(parseJudgeScores('{"other": 1}', keyToId).size, 0);
}
console.log("ok parseJudgeScores");

// ── JudgmentCache: 永続化と読み戻し ─────────────────────────────────────────
{
  const dir = path.resolve(".tmp/eval-test");
  fs.rmSync(dir, { recursive: true, force: true });
  const file = path.join(dir, "judgments.json");

  const cache = new JudgmentCache(file);
  assert.equal(cache.get("m", "テーマ", "id-a"), undefined);
  cache.set("m", "テーマ", "id-a", 2);
  cache.save();

  const reloaded = new JudgmentCache(file);
  assert.equal(reloaded.get("m", "テーマ", "id-a"), 2);
  assert.equal(reloaded.get("other-model", "テーマ", "id-a"), undefined, "モデル別に分離");

  // 壊れたキャッシュファイルは作り直す (throw しない)。
  fs.writeFileSync(file, "broken json", "utf8");
  const rebuilt = new JudgmentCache(file);
  assert.equal(rebuilt.get("m", "テーマ", "id-a"), undefined);
  fs.writeFileSync(file, '{"judgments":{"m":null}}', "utf8");
  const invalidShape = new JudgmentCache(file);
  assert.equal(invalidShape.get("m", "テーマ", "id-a"), undefined, "不正な形のキャッシュも作り直す");
  fs.rmSync(dir, { recursive: true, force: true });
}
console.log("ok JudgmentCache");

// ── judgeRelevance: キャッシュ優先 + 未採点だけ LLM ──────────────────────────
{
  const dir = path.resolve(".tmp/eval-test2");
  fs.rmSync(dir, { recursive: true, force: true });
  const cache = new JudgmentCache(path.join(dir, "judgments.json"));
  cache.set("model-x", "テーマ", "cached-id", 1);

  let invoked = 0;
  const mockLlm: LLMClient = {
    async invoke(args) {
      invoked += 1;
      // v1..vN 全てに 2 を返す。
      const keys = [...(args.prompt.matchAll(/^(v\d+):/gm))].map((m) => m[1]);
      const scores = Object.fromEntries(keys.map((k) => [k, 2]));
      return { ok: true, text: JSON.stringify({ scores }) };
    },
  };

  const result = await judgeRelevance({
    llm: mockLlm,
    theme: "テーマ",
    items: [
      { id: "cached-id", content: "採点済み" },
      { id: "new-id", content: "未採点" },
    ],
    cache,
    cacheModelKey: "model-x",
  });
  assert.equal(invoked, 1, "未採点分だけ 1 コール");
  assert.equal(result.get("cached-id"), 1, "キャッシュ値を再採点しない");
  assert.equal(result.get("new-id"), 2);

  // 2 回目は全てキャッシュ命中で LLM を呼ばない。
  const again = await judgeRelevance({
    llm: mockLlm,
    theme: "テーマ",
    items: [
      { id: "cached-id", content: "採点済み" },
      { id: "new-id", content: "未採点" },
    ],
    cache,
    cacheModelKey: "model-x",
  });
  assert.equal(invoked, 1);
  assert.equal(again.get("new-id"), 2);
  fs.rmSync(dir, { recursive: true, force: true });
}
console.log("ok judgeRelevance");

console.log("relevance-judge tests: all passed");
