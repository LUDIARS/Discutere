// Root package.json の `file:` 依存を node_modules へリンクする。
//
// setup:submodules は各 submodule パッケージを build するだけで、ルートの
// node_modules は触らない。依存表に `file:` パッケージを足したあと (例: @ludiars/one-shot)
// ルートで npm install をやり直さずに再起動すると、リンクが無く ERR_MODULE_NOT_FOUND で
// 起動しない。ここで不足分だけリンクを張り、既存のリンク・実体は触らない。

import fs from "node:fs";
import path from "node:path";

/**
 * リンクが要る `file:` 依存を列挙する (純粋関数)。
 * @param {Record<string, unknown>} manifest ルート package.json
 * @param {string} repoRoot
 * @param {(p: string) => boolean} exists
 * @returns {{ name: string, target: string, link: string }[]}
 */
export function planFileDependencyLinks(manifest, repoRoot, exists) {
  const sections = [manifest.dependencies, manifest.devDependencies, manifest.optionalDependencies];
  const plans = [];
  const seen = new Set();
  for (const section of sections) {
    if (section === null || typeof section !== "object") continue;
    for (const [name, spec] of Object.entries(section)) {
      if (seen.has(name) || typeof spec !== "string" || !spec.startsWith("file:")) continue;
      seen.add(name);
      const target = path.resolve(repoRoot, spec.slice("file:".length));
      const link = path.join(repoRoot, "node_modules", ...name.split("/"));
      if (!exists(path.join(target, "package.json"))) continue;
      if (exists(link)) continue;
      plans.push({ name, target, link });
    }
  }
  return plans;
}

/** 計画どおりにリンクを張る。Windows は管理者権限の要らない junction を使う。 */
export function linkFileDependencies(repoRoot) {
  const manifest = JSON.parse(fs.readFileSync(path.join(repoRoot, "package.json"), "utf8"));
  const plans = planFileDependencyLinks(manifest, repoRoot, (p) => fs.existsSync(p));
  for (const plan of plans) {
    fs.mkdirSync(path.dirname(plan.link), { recursive: true });
    fs.symlinkSync(plan.target, plan.link, process.platform === "win32" ? "junction" : "dir");
    console.log(`linked ${plan.name} -> ${path.relative(repoRoot, plan.target)}`);
  }
  return plans;
}
