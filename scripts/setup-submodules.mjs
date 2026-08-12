#!/usr/bin/env node
// LUDIARS 依存 (llm-gateway / blackbox / sentiment-core / canalis / vestigium / fundamentum) を
// git submodule (lib/*) として取得・ビルドする。GitHub Packages の NODE_AUTH_TOKEN は不要。
// @spec sentiment 空間の一本化
import { execFileSync, spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(fileURLToPath(new URL(".", import.meta.url)), "..");

function directInvocation(cmd, args) {
  if (process.platform === "win32" && cmd === "npm") {
    const npmCli = process.env.npm_execpath;
    if (!npmCli) {
      throw new Error("Windows では `npm run setup:submodules` 経由で実行してください");
    }
    return { executable: process.execPath, args: [npmCli, ...args] };
  }
  return { executable: cmd, args };
}

// submodule 更新や node_modules 操作より先に Windows の npm 起動契約を検証する。
if (process.platform === "win32") directInvocation("npm", []);

function run(cmd, args, cwd) {
  console.log(`\n$ ${cmd} ${args.join(" ")}  (cwd: ${path.relative(repoRoot, cwd) || "."})`);
  const invocation = directInvocation(cmd, args);
  execFileSync(invocation.executable, invocation.args, {
    cwd,
    stdio: "inherit",
    env: {
      ...process.env,
      // canalis が playwright に依存しており、postinstall のブラウザバイナリ DL
      // (数百 MB) がコールドキャッシュ環境で審査ゲートの 600s を食い潰す。
      // ここはライブラリとしての install+build だけなのでブラウザは不要。
      // 実ブラウザが要る利用側 (crawl 実行) は各自 `npx playwright install` する。
      PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD: "1",
    },
  });
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

// audit/fund は不要な外部アクセス・出力なので切る。prefer-offline でキャッシュ優先。
// NODE_ENV=production だと devDependencies (typescript 等) が入らず tsc が失敗するため --include=dev を明示。
const INSTALL_ARGS = ["install", "--include=dev", "--package-lock=false", "--no-audit", "--no-fund", "--prefer-offline"];
const CI_ARGS = ["ci", "--include=dev", "--no-audit", "--no-fund", "--prefer-offline"];

/**
 * npm install (残骸耐性つき)。前回中断した install の node_modules 残骸
 * (例: kuzu の kuzu-source) が Windows で ENOTEMPTY を出して失敗することがある
 * (審査環境は temp 作業ディレクトリを再利用するため実際に発生した)。
 * 失敗したら node_modules を消して 1 回だけやり直す。
 */
async function npmInstallResilient(cwd) {
  // Committed locks avoid npm's expensive dependency-resolution pass in the
  // larger submodules. Lapilli package directories intentionally have no npm
  // lockfiles, so they retain the install path above.
  const installArgs = fs.existsSync(path.join(cwd, "package-lock.json")) ? CI_ARGS : INSTALL_ARGS;

  try {
    await runAsync("npm", installArgs, cwd);
  } catch (err) {
    if (!(err instanceof Error) || !err.message.startsWith("npm exited with code")) throw err;
    const nodeModules = path.join(cwd, "node_modules");
    const displayPath = path.relative(repoRoot, nodeModules);
    console.warn(`npm install failed (${err.message}); ${displayPath} を除去して 1 回だけ再試行`);
    fs.rmSync(nodeModules, { recursive: true, force: true, maxRetries: 10, retryDelay: 500 });
    await runAsync("npm", installArgs, cwd);
  }
}

function runAsync(cmd, args, cwd) {
  console.log(`\n$ ${cmd} ${args.join(" ")}  (cwd: ${path.relative(repoRoot, cwd) || "."})`);
  return new Promise((resolve, reject) => {
    const invocation = directInvocation(cmd, args);
    const child = spawn(invocation.executable, invocation.args, {
      cwd,
      stdio: "inherit",
      env: { ...process.env, PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD: "1" },
    });
    child.once("error", reject);
    child.once("close", (code) => {
      if (code === 0) resolve();
      else reject(new Error(`${cmd} exited with code ${code}`));
    });
  });
}

async function setupPackage(relDir) {
  const cwd = path.resolve(repoRoot, relDir);
  await npmInstallResilient(cwd);
  await runAsync("npm", ["run", "build"], cwd);
}

// Canalis is the only expensive cold install. Overlap it with the remaining
// independent packages without starting several competing npm downloads.
const canalisDirectory = "lib/canalis";
let setupFailure = null;
async function setupPackageGuarded(relDir) {
  if (setupFailure) return;
  try {
    await setupPackage(relDir);
  } catch (err) {
    setupFailure ??= err;
  }
}

await Promise.all([
  setupPackageGuarded(canalisDirectory),
  (async () => {
    for (const relDir of packageDirectories) {
      if (setupFailure) break;
      if (relDir !== canalisDirectory) await setupPackageGuarded(relDir);
    }
  })(),
]);
if (setupFailure) throw setupFailure;

console.log("\nsubmodule のセットアップが完了しました。続けて `npm install` を実行してください。");
