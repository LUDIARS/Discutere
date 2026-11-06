// バックアップの tar.gz 作成 (Node のまま残す: tar パッケージを自作しないため)。
// 入力 (stdin JSON): { targets: string[], outFile: string, cwd?: string }
// 出力 (stdout 1 行): { ok: true, result: { outFile, included, skipped } } | { ok: false, error }
// C++ 側は native/src/backup/archive.cpp の create_archive から di::sidecar::run で呼ぶ。
import { createArchive } from "../../src/backup/archive.js";

const MAX_INPUT_BYTES = 1 << 20;

async function readStdin(): Promise<string> {
  const chunks: Buffer[] = [];
  let size = 0;
  for await (const chunk of process.stdin) {
    const buf = chunk as Buffer;
    size += buf.length;
    if (size > MAX_INPUT_BYTES) throw new Error("input too large");
    chunks.push(buf);
  }
  return Buffer.concat(chunks).toString("utf8");
}

function fail(error: string): void {
  process.stdout.write(JSON.stringify({ ok: false, error }) + "\n");
}

try {
  const input = JSON.parse(await readStdin()) as unknown;
  if (!input || typeof input !== "object") throw new Error("input must be an object");
  const { targets, outFile, cwd } = input as Record<string, unknown>;
  if (!Array.isArray(targets) || targets.length > 1000 || !targets.every((t) => typeof t === "string")) {
    throw new Error("targets must be an array of strings");
  }
  if (typeof outFile !== "string" || !outFile) throw new Error("outFile must be a string");
  if (cwd !== undefined && typeof cwd !== "string") throw new Error("cwd must be a string");
  const result = await createArchive({ targets: targets as string[], outFile, cwd: cwd as string | undefined });
  process.stdout.write(JSON.stringify({ ok: true, result }) + "\n");
} catch (e) {
  fail((e as Error).message);
}
