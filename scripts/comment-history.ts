import { writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { loadHistorySettings } from "../src/crawler/comment-history/config.js";
import { collectHistory } from "../src/crawler/comment-history/runner.js";
import { HistoryStore } from "../src/crawler/comment-history/store.js";

async function main(): Promise<void> {
  const [command, configPath, output, ...extra] = process.argv.slice(2);
  if (!configPath || extra.length || !["collect", "export"].includes(command) ||
    (command === "collect" && output) || (command === "export" && !output)) {
    throw new Error("Usage: npm run comment-history -- collect <config.json> | export <config.json> <snapshot.json>");
  }
  const settings = loadHistorySettings(resolve(configPath), process.env, command === "collect");
  if (command === "collect") process.stdout.write(`${JSON.stringify(await collectHistory(settings))}\n`);
  else {
    if (resolve(output) === settings.database) throw new Error("Output must not overwrite the history database");
    const store = new HistoryStore(settings.database);
    try {
      const snapshot = store.snapshot(Date.now());
      // Do not silently replace an older export: its consumer owns its deletion lifecycle.
      writeFileSync(resolve(output), `${JSON.stringify(snapshot)}\n`, { encoding: "utf8", flag: "wx", mode: 0o600 });
      process.stdout.write(`${JSON.stringify({ records: snapshot.records.length, expiresAt: snapshot.expiresAt })}\n`);
    } finally { store.close(); }
  }
}

main().catch(error => {
  process.stderr.write(`${error instanceof Error ? error.message : "History command failed"}\n`);
  process.exitCode = 1;
});
