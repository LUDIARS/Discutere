/**
 * FT 教師データ生成ランナー — spec/feature/gemma-ft.md (task #139)。
 *
 * headless 議論モードを N セッション自動実行し worker-turns に FT データを蓄積する。
 * 完了後 export-ft-data.ts を呼んで data/ft-export/ に JSONL を書き出す。
 *
 * 使い方:
 *   npx tsx scripts/generate-training-data.ts                        # 100 sessions / cli
 *   npx tsx scripts/generate-training-data.ts --sessions 10          # 10 sessions
 *   npx tsx scripts/generate-training-data.ts --llm anthropic        # Anthropic SDK 直接
 *   npx tsx scripts/generate-training-data.ts --rounds 30            # 1 セッションあたりのラウンド数
 *   npx tsx scripts/generate-training-data.ts --dry-run              # 1 session だけ試す
 */

import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { execSync } from "node:child_process";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, "..");

function parseArgs(argv: string[]): { sessions: number; llm: string; rounds: number; dryRun: boolean } {
  const get = (name: string): string | undefined => {
    const i = argv.indexOf(`--${name}`);
    return i >= 0 ? argv[i + 1] : undefined;
  };
  return {
    sessions: Number(get("sessions") ?? 100),
    llm: get("llm") ?? "cli",
    rounds: Number(get("rounds") ?? 40),
    dryRun: argv.includes("--dry-run"),
  };
}

function runHeadless(llm: string, rounds: number): Promise<{ ok: boolean; stdout: string }> {
  return new Promise((resolve) => {
    const child = spawn(
      "npm",
      ["run", "headless", "--", `--llm`, llm, `--rounds`, String(rounds), "--persist"],
      {
        cwd: ROOT,
        stdio: ["ignore", "pipe", "pipe"],
        shell: false,
      },
    );

    let stdout = "";
    child.stdout.on("data", (d: Buffer) => { stdout += d.toString(); });
    child.stderr.on("data", (d: Buffer) => { stdout += d.toString(); });

    child.on("close", (code) => {
      resolve({ ok: code === 0, stdout });
    });
    child.on("error", (err) => {
      resolve({ ok: false, stdout: err.message });
    });
  });
}

async function main(): Promise<void> {
  const { sessions, llm, rounds, dryRun } = parseArgs(process.argv.slice(2));
  const targetSessions = dryRun ? 1 : sessions;

  console.log(`FT data generation: sessions=${targetSessions} llm=${llm} rounds=${rounds}${dryRun ? " [dry-run]" : ""}`);

  let passed = 0;
  let failed = 0;

  for (let i = 0; i < targetSessions; i++) {
    process.stdout.write(`  session ${i + 1}/${targetSessions}... `);
    const result = await runHeadless(llm, rounds);
    if (result.ok) {
      passed++;
      console.log("ok");
    } else {
      failed++;
      console.log(`FAILED\n${result.stdout.slice(-300)}`);
    }
  }

  console.log(`\n完了: ${passed} ok / ${failed} failed`);

  // export 実行
  console.log("\nexport-ft-data を実行...");
  try {
    execSync("npm run ft:export", { cwd: ROOT, stdio: "inherit" });
  } catch {
    console.error("export-ft-data の実行に失敗しました");
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
