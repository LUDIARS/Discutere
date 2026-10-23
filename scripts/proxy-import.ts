/**
 * 代理議論の人物パックを取り込む (spec/feature/flow/proxy-debate.md §3)。
 *
 *   npm run proxy:import -- --input <packs.jsonl|packs.json>
 *
 * 契約に通らない行は理由付きで表示し、残りは取り込む。終了コードは拒否があれば 1。
 */

import { readFileSync } from "node:fs";
import path from "node:path";

import { importPersonaPacks, parsePackInput } from "../src/flow/proxy/import.js";

function valueOf(flag: string): string | undefined {
  const index = process.argv.indexOf(flag);
  const value = index >= 0 ? process.argv[index + 1] : undefined;
  return value && !value.startsWith("--") ? value : undefined;
}

const input = valueOf("--input");
if (!input) {
  console.error("usage: npm run proxy:import -- --input <packs.jsonl|packs.json>");
  process.exit(2);
}

const rows = parsePackInput(readFileSync(path.resolve(input), "utf8"));
const summary = importPersonaPacks(rows);
console.log(
  `proxy:import 取込 ${rows.length} 行: 追加 ${summary.inserted} / 更新 ${summary.updated} / 変化なし ${summary.unchanged} / ` +
    `失効 ${summary.revoked} / 拒否 ${summary.rejected.length}`
);
for (const r of summary.rejected) {
  console.log(`  行 ${r.index + 1}: ${r.errors.slice(0, 5).join(" / ")}${r.errors.length > 5 ? ` 他 ${r.errors.length - 5} 件` : ""}`);
}
process.exit(summary.rejected.length > 0 ? 1 : 0);
