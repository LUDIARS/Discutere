/**
 * フロー T1-T7 テスト一括 runner。
 *
 *   npm run test:flow
 *
 * 各テストファイルはグローバルな DATABASE_PATH / DB 接続を書き換えるため、
 * static import を使うと top-level await が並行してしまい競合する。
 * 順次 dynamic import で完全逐次実行を保証する。
 */
await import("./persona-pool.test.js");
await import("./claude-code-auth.test.js");
await import("./persona-import.test.js");
await import("./persona-display.test.js");
await import("./worker-pool-usage.test.js");
await import("./spec-source.test.js");

console.log("flow tests: all passed");
