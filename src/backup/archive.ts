/**
 * 学習データの tar.gz アーカイブ生成.
 *
 * Discatier KG (kuzu) + persona-engine.db + discutere.db を 1 本の tar.gz にまとめる。
 * 存在しないターゲットは skip し、最低 1 件あればアーカイブする (= 初回起動直後でも壊れない)。
 * cwd 相対パスで格納するので、展開すれば元のレイアウトに復元できる。
 */

import * as tar from "tar";
import { existsSync } from "node:fs";
import path from "node:path";

export interface CreateArchiveArgs {
  /** アーカイブ対象 (ファイル or ディレクトリのパス) */
  targets: string[];
  /** 出力 tar.gz パス */
  outFile: string;
  /** 相対パス基準 (既定 process.cwd()) */
  cwd?: string;
}

export interface CreateArchiveResult {
  outFile: string;
  /** 実際に含めた cwd 相対パス */
  included: string[];
  /** 存在せず skip したターゲット */
  skipped: string[];
}

export async function createArchive(args: CreateArchiveArgs): Promise<CreateArchiveResult> {
  const cwd = args.cwd ?? process.cwd();
  const included: string[] = [];
  const skipped: string[] = [];

  for (const t of args.targets) {
    const abs = path.resolve(cwd, t);
    if (existsSync(abs)) {
      // tar はスラッシュ区切りを期待するので Windows 区切りを正規化
      included.push(path.relative(cwd, abs).split(path.sep).join("/"));
    } else {
      skipped.push(t);
    }
  }

  if (included.length === 0) {
    throw new Error(`createArchive: アーカイブ対象が 1 件も存在しません (targets=${args.targets.join(", ")})`);
  }

  await tar.create({ gzip: true, file: args.outFile, cwd, portable: true }, included);
  return { outFile: args.outFile, included, skipped };
}
