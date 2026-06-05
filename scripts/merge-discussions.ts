/**
 * 議論データのマージ (worktree → 本体)。
 *
 * 自動シード議論は worktree の data/ に蓄積されるため、 worktree を撤去すると消える。
 * このスクリプトで worktree 側 DB の行を本体 (別 clone) の DB に取り込む。
 *
 *   node_modules/.bin/tsx scripts/merge-discussions.ts            # data/ → ../Discutere/data/
 *   node_modules/.bin/tsx scripts/merge-discussions.ts --dry      # 取り込み件数だけ表示
 *   node_modules/.bin/tsx scripts/merge-discussions.ts --from <dir> --to <dir>
 *
 * 方式: ATTACH したソース DB の各テーブルを INSERT OR IGNORE で取り込む。
 * 主キーは UUID なので衝突せず、 同じ DB を重ねて流しても二重取り込みされない (冪等)。
 * 対象は両 DB に同名で存在するテーブルのみ、 共通カラムだけをコピーする。
 */

import Database from "better-sqlite3";
import fs from "node:fs";
import path from "node:path";

function arg(name: string, def?: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : def;
}

const DB_FILES = ["discatier.kuzu", "persona-engine.db"];
const SKIP_TABLES = new Set(["_migrations", "_pe_migrations", "_sqlx_migrations"]);

interface TableReport {
  table: string;
  copied: number;
  created: boolean;
}

function tableColumns(db: Database.Database, schema: string, table: string): string[] {
  const rows = db
    .prepare("SELECT name FROM pragma_table_info(?, ?)")
    .all(table, schema) as Array<{ name: string }>;
  return rows.map((r) => r.name);
}

function mergeOneDb(fromFile: string, toFile: string, dryRun: boolean): TableReport[] {
  const target = new Database(toFile);
  target.pragma("busy_timeout = 15000");
  target.exec(`ATTACH DATABASE '${fromFile.replace(/'/g, "''")}' AS src`);
  const reports: TableReport[] = [];
  try {
    const srcTables = target
      .prepare("SELECT name FROM src.sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'")
      .all() as Array<{ name: string }>;
    for (const { name } of srcTables) {
      if (SKIP_TABLES.has(name)) continue;
      const exists = target
        .prepare("SELECT 1 FROM main.sqlite_master WHERE type='table' AND name=?")
        .get(name);
      let created = false;
      if (!exists) {
        const ddl = target
          .prepare("SELECT sql FROM src.sqlite_master WHERE type='table' AND name=?")
          .get(name) as { sql: string } | undefined;
        if (!ddl?.sql) continue;
        if (!dryRun) target.exec(ddl.sql);
        created = true;
      }
      const srcCols = tableColumns(target, "src", name);
      const tgtCols = exists ? tableColumns(target, "main", name) : srcCols;
      const cols = srcCols.filter((c) => tgtCols.includes(c));
      if (cols.length === 0) continue;
      const collist = cols.map((c) => `"${c}"`).join(", ");
      const before = created
        ? 0
        : ((target.prepare(`SELECT COUNT(*) AS n FROM main."${name}"`).get() as { n: number }).n);
      if (!dryRun) {
        target.exec(
          `INSERT OR IGNORE INTO main."${name}" (${collist}) SELECT ${collist} FROM src."${name}"`
        );
      }
      const after = dryRun
        ? before +
          ((target.prepare(`SELECT COUNT(*) AS n FROM src."${name}"`).get() as { n: number }).n)
        : ((target.prepare(`SELECT COUNT(*) AS n FROM main."${name}"`).get() as { n: number }).n);
      reports.push({ table: name, copied: dryRun ? -1 : after - before, created });
    }
  } finally {
    target.exec("DETACH DATABASE src");
    target.close();
  }
  return reports;
}

function main(): void {
  const dryRun = process.argv.includes("--dry");
  const fromDir = path.resolve(arg("from", "./data")!);
  const toDir = path.resolve(arg("to", "../Discutere/data")!);
  console.log(`# merge-discussions ${dryRun ? "(dry-run)" : ""}`);
  console.log(`  from: ${fromDir}`);
  console.log(`  to  : ${toDir}\n`);

  for (const file of DB_FILES) {
    const fromFile = path.join(fromDir, file);
    const toFile = path.join(toDir, file);
    if (!fs.existsSync(fromFile)) {
      console.log(`- ${file}: source 無し → skip`);
      continue;
    }
    if (!fs.existsSync(toFile)) {
      console.log(`- ${file}: target 無し → 丸ごとコピー`);
      if (!dryRun) fs.copyFileSync(fromFile, toFile);
      continue;
    }
    console.log(`- ${file}:`);
    const reports = mergeOneDb(fromFile, toFile, dryRun);
    for (const r of reports) {
      const tag = r.created ? " (新規テーブル)" : "";
      console.log(`    ${r.table}: ${dryRun ? "+? (dry)" : "+" + r.copied}${tag}`);
    }
  }
  console.log(`\n${dryRun ? "dry-run 完了 (変更なし)" : "merge 完了"}`);
}

main();
