import { readFileSync } from "node:fs";
import path from "node:path";

import { importVoluptasPersonas } from "../src/flow/persona-import.js";
import { parsePersonaDocument } from "../src/flow/persona-import-document.js";
import { pullVoluptasPersonas } from "../src/flow/voluptas-persona-client.js";

// @spec インポート (Vo → Di)
function valueOf(flag: string): string | undefined {
  const index = process.argv.indexOf(flag);
  const value = index >= 0 ? process.argv[index + 1] : undefined;
  return value && !value.startsWith("--") ? value : undefined;
}

async function loadInput(): Promise<{
  personas: unknown[];
  invalidJsonLines: number;
  pages?: number;
}> {
  const input = valueOf("--input") ?? (
    process.argv[2] && !process.argv[2].startsWith("--") ? process.argv[2] : undefined
  );
  const url = valueOf("--url");
  if (!!input === !!url) {
    throw new Error("use exactly one of --input <path> or --url <Voluptas export URL>");
  }
  if (input) return parsePersonaDocument(readFileSync(path.resolve(input), "utf8"));
  return pullVoluptasPersonas({
    url: url!,
    token: process.env.DISCUTERE_VOLUPTAS_EXPORT_TOKEN ?? "",
  });
}

async function main(): Promise<void> {
  const source = await loadInput();
  const summary = importVoluptasPersonas(source.personas, {
    dryRun: process.argv.includes("--dry"),
    invalidJsonLines: source.invalidJsonLines,
  });
  process.stdout.write(`${JSON.stringify({
    ...summary,
    ...(source.pages === undefined ? {} : { pages: source.pages }),
  })}\n`);
}

main().catch((error) => {
  process.stderr.write(`persona import failed: ${(error as Error).message}\n`);
  process.exitCode = 1;
});
