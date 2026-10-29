// lib/fundamentum (Node 実装) の入出力を記録する。実行: npx tsx native/tests/golden/w1-A-fundamentum.gen.ts
// 出力: native/tests/fixtures/fundamentum/canonical.json ({ input, canonical, id }[])
//       native/tests/fixtures/fundamentum/scenario.json (FileBackend の操作列の後のファイル一覧と中身 + 戻り値)
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { canonicalize, contentIdOf, Foundation } from "../../../lib/fundamentum/src/index.js";

const values: unknown[] = [
  null, true, false, 0, -0, 1, -1.5, 0.1, 1e21, 1e-7, 123456789012, 2 ** 53 + 2, "", "abc", "日本語\n\"q\"\t\\",
  " \u0001", [], {}, [1, [2, [3]]], { b: 1, a: 2 }, { z: { y: 1, x: 2 }, a: [3, { d: 4, c: 5 }] },
  { "～": 1, "\u{1F600}": 2, "A": 3, "a": 4, "10": 5, "9": 6, "": 7 },
  { nested: { list: [{ k: "v" }, null, true], num: 3.14159 } },
];
const canonical = values.map((input) => ({ input, canonical: canonicalize(input as never), id: contentIdOf(input as never) }));

const dir = fs.mkdtempSync(path.join(os.tmpdir(), "di-golden-fm-"));
const fm = Foundation.onDisk(dir);
const results: Record<string, unknown> = {};
const r1 = await fm.master.put({ title: "議論A", utterances: [{ text: "x" }] });
const r2 = await fm.master.put({ title: "議論B" });
const r3 = await fm.master.put({ title: "議論A", utterances: [{ text: "x" }] });
results.puts = [r1.id, r2.id, r3.id];
const v1 = await fm.catalog.builder("discutere/discussions").set("flow:s1", r1.id).set("gap-2", r2.id).commit();
const v1b = await fm.catalog.builder("discutere/discussions").set("flow:s1", r1.id).commit();
const r4 = await fm.master.put({ title: "議論A2" });
const v2 = await fm.catalog.builder("discutere/discussions").set("flow:s1", r4.id).remove("gap-2").set("新規", r2.id).commit();
results.catalogs = [v1, v1b, v2];
results.diff = fm.catalog.diff(v1, v2);
results.history = (await fm.catalog.history("discutere/discussions")).map((c) => c.id);
results.version1 = (await fm.catalog.version("discutere/discussions", 1))?.id ?? null;
results.names = await fm.catalog.names();
results.userSet1 = await fm.user.set("proj-1", "flow:s1", { note: "上書き" });
results.userSet2 = await fm.user.set("proj-1", "flow:s1", { note: "二回目" });
results.userDel = await fm.user.delete("proj-1", "gap-x");
results.userList = await fm.user.list("proj-1");
results.resolveAll = await fm.resolveAll("proj-1", "discutere/discussions");
results.resolveNone = await fm.resolve("proj-1", "discutere/discussions", "missing");

function walk(d: string, base = ""): Record<string, string> {
  const out: Record<string, string> = {};
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const rel = base ? `${base}/${e.name}` : e.name;
    if (e.isDirectory()) Object.assign(out, walk(path.join(d, e.name), rel));
    else out[rel] = fs.readFileSync(path.join(d, e.name), "utf8");
  }
  return out;
}
const files = walk(dir);
fs.rmSync(dir, { recursive: true, force: true });

const outDir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "fundamentum");
fs.mkdirSync(outDir, { recursive: true });
fs.writeFileSync(path.join(outDir, "canonical.json"), JSON.stringify(canonical, null, 2) + "\n");
fs.writeFileSync(path.join(outDir, "scenario.json"), JSON.stringify({ results, files }, null, 2) + "\n");
console.log(`wrote ${canonical.length} canonical cases, ${Object.keys(files).length} files`);
