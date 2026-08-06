import { mkdirSync, renameSync, writeFileSync } from "node:fs";
import path from "node:path";

import { estimatePopulationReport } from "../src/flow/persona-populations.js";
import type { PersonaOrigin } from "../src/flow/persona-pool.js";

// @spec 母集団レポート (Di → Vo)
function valueOf(flag: string): string | undefined {
  const index = process.argv.indexOf(flag);
  const value = index >= 0 ? process.argv[index + 1] : undefined;
  return value && !value.startsWith("--") ? value : undefined;
}

function numberValue(flag: string): number | undefined {
  const raw = valueOf(flag);
  if (raw === undefined) return undefined;
  const value = Number(raw);
  if (!Number.isFinite(value) || value < 0 || value > 1) {
    throw new Error(`${flag} must be in 0..1`);
  }
  return value;
}

const ORIGINS: readonly PersonaOrigin[] = ["seed", "adopted", "imported"];

function originsValue(flag: string): PersonaOrigin[] | undefined {
  const raw = valueOf(flag);
  if (raw === undefined) return undefined;
  const origins = raw.split(",").map((item) => item.trim()).filter(Boolean);
  const unknown = origins.filter((origin) => !ORIGINS.includes(origin as PersonaOrigin));
  if (origins.length === 0 || unknown.length > 0) {
    throw new Error(`${flag} must be a comma separated subset of ${ORIGINS.join(",")}`);
  }
  return origins as PersonaOrigin[];
}

function main(): void {
  const output = valueOf("--report");
  if (!output) {
    throw new Error("usage: npm run persona:populations -- --report <population-report.json>");
  }
  const report = estimatePopulationReport({
    similarityThreshold: numberValue("--threshold"),
    majorRatio: numberValue("--major-ratio"),
    realOrigins: originsValue("--real"),
  });
  const filePath = path.resolve(output);
  const temporaryPath = `${filePath}.${process.pid}.tmp`;
  mkdirSync(path.dirname(filePath), { recursive: true });
  writeFileSync(temporaryPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
  renameSync(temporaryPath, filePath);
  process.stdout.write(`${JSON.stringify({
    output: filePath,
    realPopulation: report.realPopulation,
    entries: report.entries.length,
  })}\n`);
}

try {
  main();
} catch (error) {
  process.stderr.write(`population report failed: ${(error as Error).message}\n`);
  process.exitCode = 1;
}
