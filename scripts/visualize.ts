/**
 * Visualize CLI (Phase 0)
 *
 *   npm run visualize <type> <id> [--workspace <ws>] [--root <dir>]
 *
 *   type: hypothesis|hyp / gap / mechanic|mch / aesthetic|aes / utterance|utt / session|ses
 *
 * 出力: data/discussions/<workspace>/<type-dir>/<id>.md
 *   workspace = --workspace flag > env DISCATIER_WORKSPACE > "knowledge"
 *
 * DB path は env DISCATIER_KUZU_PATH > "./data/discatier.kuzu" の順
 * (createCore の default に従う)。
 */

import path from "node:path";

import { createCore } from "../src/core/index.js";
import { dumpNode, normalizeType } from "../src/visualize/index.js";

function main(): void {
  const argv = process.argv.slice(2);
  if (argv.length < 2) {
    printUsageAndExit();
  }

  const [typeArg, idArg, ...rest] = argv;
  const type = normalizeType(typeArg);
  if (!type) {
    console.error(`unknown type: ${typeArg}`);
    printUsageAndExit();
  }
  if (!idArg || idArg.startsWith("--")) {
    console.error("id is required");
    printUsageAndExit();
  }

  const flags = parseFlags(rest);
  const workspaceId =
    flags.workspace ?? process.env.DISCATIER_WORKSPACE ?? "knowledge";
  const rootDir = flags.root ? path.resolve(flags.root) : undefined;

  const core = createCore();
  try {
    const result = dumpNode(core, type, idArg, { workspaceId, rootDir });
    console.log(
      JSON.stringify(
        {
          type: result.type,
          id: result.id,
          file: path.relative(process.cwd(), result.filePath),
          written: result.written,
          outgoing: result.outgoing.length,
        },
        null,
        2
      )
    );
  } finally {
    core.close();
  }
}

function parseFlags(args: string[]): { workspace?: string; root?: string } {
  const out: { workspace?: string; root?: string } = {};
  for (let i = 0; i < args.length; i += 1) {
    const a = args[i];
    if (a === "--workspace" || a === "-w") {
      out.workspace = args[i + 1];
      i += 1;
    } else if (a === "--root") {
      out.root = args[i + 1];
      i += 1;
    }
  }
  return out;
}

function printUsageAndExit(): never {
  console.error(
    "usage: visualize.ts <type> <id> [--workspace <ws>] [--root <dir>]\n  type: hypothesis|hyp / gap / mechanic|mch / aesthetic|aes / utterance|utt / session|ses"
  );
  process.exit(2);
}

main();
