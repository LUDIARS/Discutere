/**
 * フロー T1-T7 テスト一括 runner。
 *
 *   npm run test:flow
 *
 * 各テストファイルはグローバルな DATABASE_PATH / DB 接続を書き換えるため、
 * static import を使うと top-level await が並行してしまい競合する。
 * 順次 dynamic import で完全逐次実行を保証する。
 */
await import("./foundation.test.js");
await import("./discussion.test.js");
await import("./vote-conclude.test.js");
await import("./improvement.test.js");
await import("./learning.test.js");
await import("./sparring.test.js");
await import("./entrypoints.test.js");
await import("./persona-pool.test.js");
await import("./claude-code-auth.test.js");
await import("./persona-survey.test.js");
await import("./persona-display.test.js");
await import("./voice-cache.test.js");
await import("./possession-vote.test.js");
await import("./learning-autocrawl.test.js");
await import("./information-gate.test.js");
await import("./cost-instrumentation.test.js");
await import("./cost-relay.test.js");
await import("./worker-pool-usage.test.js");
await import("./cost-estimate.test.js");
await import("./paper-markdown.test.js");
await import("./paper-blocks.test.js");
await import("./paper-revisions.test.js");
await import("./paper-review.test.js");
await import("./mechanic-extract.test.js");
await import("./paper-refine.test.js");
await import("./spec-analyze.test.js");
await import("./spec-source.test.js");
await import("./persona-routes.test.js");
await import("./anatomia-refine.test.js");
await import("./anatomia-client.test.js");

console.log("flow tests: all passed");
