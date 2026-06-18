/**
 * worker-pool usage 回収 (#135) のデルタ集計テスト。
 *
 * usageDeltaFromTranscript が:
 *  - assistant `message.usage` だけを拾う (user/summary/壊れ行は無視)
 *  - 前回計上分 (カーソル) より後の新規行のみ合算する (= SUM 二重計上の回避)
 *  - 新規が無ければ usage:null・count は据え置き
 */

import assert from "node:assert/strict";

import { usageDeltaFromTranscript } from "../../worker-home/scripts/usage.mjs";

const asLine = (obj: unknown): string => JSON.stringify(obj);
const assistant = (u: Record<string, number>): string =>
  asLine({ type: "assistant", message: { role: "assistant", usage: u } });

{
  // 空 transcript → usage:null, count:0
  const r = usageDeltaFromTranscript("", 0);
  assert.equal(r.usage, null);
  assert.equal(r.count, 0);
  console.log("  [ok] empty transcript → null");
}

{
  // user / summary / 壊れ行は usage に数えない
  const text = [
    asLine({ type: "user", message: { role: "user", content: "hi" } }),
    asLine({ type: "summary", summary: "..." }),
    "not json at all",
    assistant({ input_tokens: 10, output_tokens: 4, cache_read_input_tokens: 100, cache_creation_input_tokens: 20 }),
  ].join("\n");
  const r = usageDeltaFromTranscript(text, 0);
  assert.equal(r.count, 1, "1 usage-bearing assistant line");
  assert.deepEqual(r.usage, {
    input_tokens: 10,
    output_tokens: 4,
    cache_read_input_tokens: 100,
    cache_creation_input_tokens: 20,
  });
  console.log("  [ok] only assistant usage counted");
}

{
  // カーソルで前回分をスキップし、新規 2 行だけ合算する (二重計上回避)
  const text = [
    assistant({ input_tokens: 10, output_tokens: 4 }), // 既計上 (cursor=1)
    assistant({ input_tokens: 20, output_tokens: 5 }),
    assistant({ input_tokens: 30, output_tokens: 6 }),
  ].join("\n");
  const r = usageDeltaFromTranscript(text, 1);
  assert.equal(r.count, 3, "total usage lines = 3 (next cursor)");
  assert.equal(r.usage!.input_tokens, 50, "20+30 only (first skipped)");
  assert.equal(r.usage!.output_tokens, 11, "5+6 only");
  console.log("  [ok] cursor skips already-counted lines");
}

{
  // 全行計上済み → 新規なし
  const text = [assistant({ input_tokens: 10, output_tokens: 4 })].join("\n");
  const r = usageDeltaFromTranscript(text, 1);
  assert.equal(r.usage, null, "no fresh lines → null");
  assert.equal(r.count, 1, "count unchanged");
  console.log("  [ok] no new lines → null");
}

{
  // 累積を 2 回送ると合計が transcript 総量に一致する (cost-report の SUM と整合)
  const lines = [
    assistant({ input_tokens: 10, output_tokens: 4 }),
    assistant({ input_tokens: 20, output_tokens: 5 }),
  ];
  // send #1: 1 行目まで flush
  const d1 = usageDeltaFromTranscript(lines.slice(0, 1).join("\n"), 0);
  // send #2: 2 行目も flush、 カーソル = d1.count
  const d2 = usageDeltaFromTranscript(lines.join("\n"), d1.count);
  const sumIn = (d1.usage!.input_tokens ?? 0) + (d2.usage!.input_tokens ?? 0);
  assert.equal(sumIn, 30, "delta の合計 = transcript 総 input (10+20)");
  console.log("  [ok] summed deltas equal transcript total");
}

console.log("worker-pool-usage tests: all passed");
