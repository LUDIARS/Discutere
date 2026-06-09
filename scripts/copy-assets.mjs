/**
 * ビルド後資産コピー: src/ 配下の非 TypeScript ファイル (実行時に読む .json などの資産) を
 * dist/ へ同じ階層でミラーする。
 *
 * tsc は .ts/.tsx しか出力しないため、 `node dist/index.js` で起動すると
 * import.meta.url 相対で読む資産 (例 src/crawler/sentiment/lexicon.json) が dist に無く
 * ENOENT で即死する。 build = `tsc && node scripts/copy-assets.mjs` で資産も揃える。
 *
 * 対象は src 配下の .ts/.tsx **以外** すべて (.json/.md/.mjs 等)。 追加資産が増えても
 * 個別列挙不要で取りこぼさない。
 */
import { cp, mkdir, readdir, stat } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const srcDir = path.join(root, "src");
const outDir = path.join(root, "dist");

const SKIP_EXT = new Set([".ts", ".tsx"]);

/** src からの相対パスで非 TS ファイルを再帰収集する。 */
async function collectAssets(dir) {
  const out = [];
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      out.push(...(await collectAssets(full)));
    } else if (entry.isFile() && !SKIP_EXT.has(path.extname(entry.name).toLowerCase())) {
      out.push(full);
    }
  }
  return out;
}

async function main() {
  try {
    await stat(srcDir);
  } catch {
    console.error(`[copy-assets] src not found: ${srcDir}`);
    process.exit(1);
  }

  const assets = await collectAssets(srcDir);
  let copied = 0;
  for (const file of assets) {
    const rel = path.relative(srcDir, file);
    const dest = path.join(outDir, rel);
    await mkdir(path.dirname(dest), { recursive: true });
    await cp(file, dest);
    copied++;
  }
  console.log(`[copy-assets] copied ${copied} asset file(s) src -> dist`);
}

main().catch((err) => {
  console.error("[copy-assets] failed:", err);
  process.exit(1);
});
