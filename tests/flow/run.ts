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

console.log("flow tests: all passed");
