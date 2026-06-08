import assert from "node:assert/strict";
import path from "node:path";

import { resolveActiveKgPath, resolveActiveKgDir, listKnowledgeGraphs } from "../../src/core/kg-registry.js";
import type { DiscutereConfig } from "../../src/config.js";

/** discatier 部分だけ持つ最小 config (resolve* は discatier しか見ない)。 */
function cfg(discatier: Partial<DiscutereConfig["discatier"]>): DiscutereConfig {
  return { discatier } as unknown as DiscutereConfig;
}

// --- 1. レジストリ宣言 + activeKg 一致 ---
const reg = cfg({
  activeKg: "hollow",
  knowledgeGraphs: [
    { id: "default", label: "総合", kuzuPath: "./data/kg/default.kuzu" },
    { id: "hollow", label: "Hollow Knight", kuzuPath: "./data/kg/hollow.kuzu" },
  ],
});
assert.equal(resolveActiveKgPath(reg), "./data/kg/hollow.kuzu", "activeKg 一致の kuzuPath");
const graphs = listKnowledgeGraphs(reg);
assert.equal(graphs.length, 2);
assert.equal(graphs.find((g) => g.id === "hollow")!.active, true);
assert.equal(graphs.find((g) => g.id === "default")!.active, false);
console.log("ok registry active match");

// --- 2. activeKg 未指定 → "default" を採用 ---
const noActive = cfg({
  knowledgeGraphs: [
    { id: "default", kuzuPath: "./data/kg/default.kuzu" },
    { id: "x", kuzuPath: "./data/kg/x.kuzu" },
  ],
});
assert.equal(resolveActiveKgPath(noActive), "./data/kg/default.kuzu", "未指定は default");

// --- 3. activeKg 不一致 → 先頭にフォールバック ---
const mismatch = cfg({
  activeKg: "nope",
  knowledgeGraphs: [{ id: "first", kuzuPath: "./data/kg/first.kuzu" }, { id: "second", kuzuPath: "./b.kuzu" }],
});
assert.equal(resolveActiveKgPath(mismatch), "./data/kg/first.kuzu", "不一致は先頭");
assert.equal(listKnowledgeGraphs(mismatch)[0].active, true, "先頭が実効 active");
console.log("ok registry default / fallback");

// --- 4. 旧 単一 kuzuPath (完全後方互換) ---
const legacy = cfg({ kuzuPath: "./data/discatier.kuzu" });
assert.equal(resolveActiveKgPath(legacy), "./data/discatier.kuzu", "legacy kuzuPath 維持");
const legacyList = listKnowledgeGraphs(legacy);
assert.equal(legacyList.length, 1);
assert.equal(legacyList[0].id, "default");
assert.equal(legacyList[0].active, true);
console.log("ok legacy kuzuPath");

// --- 5. 何も無ければ undefined (adapter 既定) ---
const empty = cfg({});
assert.equal(resolveActiveKgPath(empty), undefined, "未設定は undefined");
assert.equal(listKnowledgeGraphs(empty).length, 0);
assert.equal(resolveActiveKgDir(empty), path.resolve("./data/external"), "dir 既定は data/external");
console.log("ok empty → undefined");

// --- 6. resolveActiveKgDir は active KG パスの dirname ---
assert.equal(resolveActiveKgDir(reg), path.resolve("./data/kg"), "sidecar は active KG ディレクトリ配下");
console.log("ok active kg dir");

console.log("kg-registry.test.ts: all passed");
