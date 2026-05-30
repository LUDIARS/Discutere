/**
 * バックアップ archive の単体テスト (S3 には触れない)。
 * createArchive が存在するファイルだけを tar.gz に含め、欠損は skip することを確認する。
 */

import { mkdtempSync, writeFileSync, existsSync, statSync, rmSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { createArchive } from "../../src/backup/archive.js";

let failed = 0;
function ok(cond: boolean, msg: string): void {
  if (cond) console.log(`ok ${msg}`);
  else {
    console.error(`FAIL ${msg}`);
    failed++;
  }
}

const dir = mkdtempSync(path.join(os.tmpdir(), "discutere-backup-test-"));
writeFileSync(path.join(dir, "a.db"), "AAA");
writeFileSync(path.join(dir, "b.kuzu"), "BBB");
const out = path.join(dir, "out.tar.gz");

const result = await createArchive({
  targets: [path.join(dir, "a.db"), path.join(dir, "b.kuzu"), path.join(dir, "missing.db")],
  outFile: out,
  cwd: dir,
});

ok(existsSync(out) && statSync(out).size > 0, "tar.gz が生成される");
ok(result.included.length === 2, "存在する 2 ファイルを含める");
ok(result.skipped.length === 1 && result.skipped[0].endsWith("missing.db"), "欠損ファイルは skip");

// 対象が全て存在しない場合は throw
let threw = false;
try {
  await createArchive({ targets: [path.join(dir, "none1"), path.join(dir, "none2")], outFile: out, cwd: dir });
} catch {
  threw = true;
}
ok(threw, "対象が 1 件も無いと throw する");

rmSync(dir, { recursive: true, force: true });

if (failed > 0) {
  console.error(`backup tests: ${failed} failed`);
  process.exit(1);
}
console.log("backup tests: all passed");
