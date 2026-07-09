#!/usr/bin/env node
// LUDIARS 依存 (llm-gateway / blackbox / canalis / vestigium / fundamentum) を
// git submodule (lib/*) として取得・ビルドする。GitHub Packages の NODE_AUTH_TOKEN は不要。
import { execFileSync } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(fileURLToPath(new URL(".", import.meta.url)), "..");

function run(cmd, args, cwd) {
  console.log(`\n$ ${cmd} ${args.join(" ")}  (cwd: ${path.relative(repoRoot, cwd) || "."})`);
  execFileSync(cmd, args, { cwd, stdio: "inherit", shell: process.platform === "win32" });
}

function buildPackage(relDir) {
  const cwd = path.resolve(repoRoot, relDir);
  // NODE_ENV=production だと devDependencies (typescript 等) が入らず tsc が失敗するため明示。
  run("npm", ["install", "--include=dev"], cwd);
  run("npm", ["run", "build"], cwd);
}

// CI では checkout アクション側で submodule を取得済みのため二重取得を避けられる。
if (!process.argv.includes("--skip-git-update")) {
  run("git", ["submodule", "update", "--init", "--recursive"], repoRoot);
}

for (const relDir of ["lib/vestigium", "lib/canalis", "lib/fundamentum"]) {
  buildPackage(relDir);
}

// Lapilli は pnpm workspace だが、必要な2パッケージは自己完結した build script を持つため
// pnpm 無しで個別に npm install/build できる。
for (const relDir of ["lib/lapilli/packages/llm-gateway", "lib/lapilli/packages/blackbox"]) {
  buildPackage(relDir);
}

console.log("\nsubmodule のセットアップが完了しました。続けて `npm install` を実行してください。");
