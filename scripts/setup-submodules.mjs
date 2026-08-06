#!/usr/bin/env node
// LUDIARS 依存 (llm-gateway / blackbox / sentiment-core / canalis / vestigium / fundamentum) を
// git submodule (lib/*) として取得・ビルドする。GitHub Packages の NODE_AUTH_TOKEN は不要。
// @spec sentiment 空間の一本化
import { execFileSync } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(fileURLToPath(new URL(".", import.meta.url)), "..");

function run(cmd, args, cwd) {
  console.log(`\n$ ${cmd} ${args.join(" ")}  (cwd: ${path.relative(repoRoot, cwd) || "."})`);
  execFileSync(cmd, args, { cwd, stdio: "inherit", shell: process.platform === "win32" });
}

// CI では checkout アクション側で submodule を取得済みのため二重取得を避けられる。
if (!process.argv.includes("--skip-git-update")) {
  run("git", ["submodule", "update", "--init", "--recursive"], repoRoot);
}

// Lapilli は pnpm workspace だが、必要なパッケージは自己完結した build script を持つため
// pnpm 無しで個別に npm install/build できる。
const packageDirectories = [
  "lib/vestigium",
  "lib/canalis",
  "lib/fundamentum",
  "lib/lapilli/packages/llm-gateway",
  "lib/lapilli/packages/blackbox",
  "lib/lapilli/packages/sentiment-core",
];

for (const relDir of packageDirectories) {
  const cwd = path.resolve(repoRoot, relDir);
  // NODE_ENV=production だと devDependencies (typescript 等) が入らず tsc が失敗するため明示。
  run("npm", ["install", "--include=dev", "--package-lock=false"], cwd);
  run("npm", ["run", "build"], cwd);
}

console.log("\nsubmodule のセットアップが完了しました。続けて `npm install` を実行してください。");
