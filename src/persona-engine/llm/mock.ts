/**
 * MockLLMClient — テスト / 開発時に LLM を呼ばずスクリプト応答を返す。
 *
 * 使い方:
 *   const llm = new MockLLMClient([
 *     () => '{"action":"skip","reasoning":"first call"}',
 *     (args) => `{"action":"propose_hypothesis","statement":"H1","reasoning":"r"}`,
 *   ]);
 *   await llm.invoke({ prompt: "..." }) // returns "{...skip...}"
 *
 * scripted を使い切ったら fallback (default { action: "skip", reasoning: "mock exhausted" })。
 */

import type { LLMClient, LLMInvokeArgs, LLMResult } from "./client.js";

export type MockResponder = (args: LLMInvokeArgs) => string | LLMResult;

export class MockLLMClient implements LLMClient {
  private callCount = 0;

  constructor(
    private readonly responders: MockResponder[],
    private readonly fallback: string = '{"action":"skip","reasoning":"mock exhausted"}'
  ) {}

  /** これまでの呼び出し回数 (テスト assertion 用) */
  get calls(): number {
    return this.callCount;
  }

  /** 直近の prompt を渡したかどうか (テスト assertion 用) */
  lastInvocation?: LLMInvokeArgs;

  async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
    this.lastInvocation = args;
    const responder = this.responders[this.callCount];
    this.callCount += 1;
    if (!responder) {
      return { ok: true, text: this.fallback };
    }
    const out = responder(args);
    if (typeof out === "string") {
      return { ok: true, text: out };
    }
    return out;
  }
}
