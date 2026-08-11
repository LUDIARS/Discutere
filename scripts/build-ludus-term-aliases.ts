/**
 * Ludus game-lexicon (中央用語辞書) → Discutere 用語別名グループ生成。
 *
 * Ludus リポの `spec/data/game-lexicon/{terms,genres,features}/*.toml` から
 * [[term]] / [[genre]] / [[feature]] ブロックの name_ja / name_en / aliases を集め、
 * `src/discatier-engine-adapter/ludus-term-aliases.ts` に別名グループ (string[][])
 * として書き出す。生成物はコミットする (実行時に Ludus リポへ依存しない)。
 *
 * 使い方:
 *   npx tsx scripts/build-ludus-term-aliases.ts <path-to-game-lexicon-dir>
 *   例: npx tsx scripts/build-ludus-term-aliases.ts <path-to-Ludus>/spec/data/game-lexicon
 *
 * TOML パーサ依存を増やさないため、この辞書の書式 (フラットな key = "value" /
 * key = ["a", "b"]) に限定した素朴なパースで読む。書式が変わったら要追従。
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const OUT_PATH = path.resolve(HERE, "../src/discatier-engine-adapter/ludus-term-aliases.ts");
/** 語彙として短すぎて LIKE 照合の誤爆源になる語は捨てる。 */
const MIN_LEN = 2;

function parseStringValue(raw: string): string | null {
  const m = raw.trim().match(/^"((?:[^"\\]|\\.)*)"$/);
  if (!m) return null;
  return m[1].replace(/\\(.)/g, "$1");
}

function parseArrayValue(raw: string): string[] {
  const m = raw.trim().match(/^\[(.*)\]$/s);
  if (!m) return [];
  const out: string[] = [];
  // 引用符区切りで素朴に列挙 (この辞書はネスト無しの文字列配列のみ)。
  const re = /"((?:[^"\\]|\\.)*)"/g;
  let hit: RegExpExecArray | null;
  while ((hit = re.exec(m[1])) !== null) out.push(hit[1].replace(/\\(.)/g, "$1"));
  return out;
}

interface LexiconEntry {
  nameJa?: string;
  nameEn?: string;
  aliases: string[];
}

/**
 * name_ja / name_en / aliases を抜く。2 形式に対応:
 *   - terms/*.toml: [[term]] ブロックの配列
 *   - genres|features/*.toml: ファイル全体が 1 エントリのフラット形式
 */
function parseLexiconToml(text: string): LexiconEntry[] {
  const entries: LexiconEntry[] = [];
  // フラット形式用の暗黙エントリ (ブロックが現れたら以降はブロック単位)。
  let current: LexiconEntry = { aliases: [] };
  entries.push(current);
  let inTripleString = false;
  for (const line of text.split(/\r?\n/)) {
    const trimmed = line.trim();
    // description = """ … """ の中身を key=value と誤読しないよう読み飛ばす。
    if (inTripleString) {
      if (trimmed.includes('"""')) inTripleString = false;
      continue;
    }
    if (/=\s*"""/.test(trimmed) && !/"""\s*.*"""/.test(trimmed)) {
      inTripleString = true;
      continue;
    }
    if (/^\[\[[a-z_]+\]\]$/.test(trimmed)) {
      current = { aliases: [] };
      entries.push(current);
      continue;
    }
    const kv = trimmed.match(/^([a-z_]+)\s*=\s*(.+)$/);
    if (!kv) continue;
    const [, key, value] = kv;
    if (key === "name_ja") current.nameJa = parseStringValue(value) ?? undefined;
    else if (key === "name_en") current.nameEn = parseStringValue(value) ?? undefined;
    else if (key === "aliases") current.aliases = parseArrayValue(value);
  }
  return entries;
}

function walkToml(dir: string, out: string[]): void {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, entry.name);
    if (entry.isDirectory()) walkToml(p, out);
    else if (entry.name.endsWith(".toml")) out.push(p);
  }
}

function collectTomlFiles(rootDir: string): string[] {
  const subdirs = ["terms", "genres", "features"];
  const files: string[] = [];
  for (const sub of subdirs) {
    const dir = path.join(rootDir, sub);
    if (fs.existsSync(dir)) walkToml(dir, files);
  }
  return files;
}

/** @implements SPEC-VOICE-RAG-HYBRID-OPS */
function main(): void {
  const lexiconDir = process.argv[2];
  if (!lexiconDir) {
    console.error("usage: tsx scripts/build-ludus-term-aliases.ts <path-to-game-lexicon-dir>");
    process.exit(1);
  }
  if (!fs.existsSync(lexiconDir)) {
    console.error(`game-lexicon ディレクトリが見つからない: ${lexiconDir}`);
    process.exit(1);
  }
  const files = collectTomlFiles(lexiconDir);
  if (files.length === 0) {
    console.error(`TOML が 1 件も見つからない (terms/genres/features を確認): ${lexiconDir}`);
    process.exit(1);
  }

  const groups: string[][] = [];
  for (const file of files) {
    const entries = parseLexiconToml(fs.readFileSync(file, "utf-8"));
    for (const e of entries) {
      const members = [...new Set(
        [e.nameJa, e.nameEn, ...e.aliases]
          .filter((s): s is string => typeof s === "string")
          .map((s) => s.trim())
          .filter((s) => s.length >= MIN_LEN)
      )];
      // 別名が 1 つしか無い語は検索拡張に寄与しない。
      if (members.length >= 2) groups.push(members);
    }
  }

  const header =
    "/**\n" +
    " * Ludus game-lexicon 由来の用語別名グループ (自動生成 — 手編集しない)。\n" +
    " *\n" +
    " * 再生成: npx tsx scripts/build-ludus-term-aliases.ts <path-to-game-lexicon-dir>\n" +
    ` * 生成元ファイル数: ${files.length} / グループ数: ${groups.length}\n` +
    " */\n\n" +
    "export const LUDUS_TERM_ALIAS_GROUPS: readonly (readonly string[])[] = ";
  fs.writeFileSync(OUT_PATH, `${header}${JSON.stringify(groups, null, 2)};\n`, "utf-8");
  console.log(`wrote ${OUT_PATH} (${groups.length} groups from ${files.length} files)`);
}

main();
