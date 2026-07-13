import { readFileSync } from "node:fs";
import path from "node:path";

import { importVoluptasPersonas } from "../src/flow/persona-import.js";

function inputPath(): string {
  const flag = process.argv.indexOf("--input");
  const value = flag >= 0 ? process.argv[flag + 1] : process.argv[2];
  if (!value || value.startsWith("--")) {
    throw new Error("usage: npm run persona:import -- --input <voluptas-personas.json>");
  }
  return path.resolve(value);
}

function main(): void {
  const raw = readFileSync(inputPath(), "utf8");
  const summary = importVoluptasPersonas(JSON.parse(raw) as unknown);
  process.stdout.write(`${JSON.stringify(summary)}\n`);
}

try {
  main();
} catch (error) {
  process.stderr.write(`persona import failed: ${(error as Error).message}\n`);
  process.exitCode = 1;
}
