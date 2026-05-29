/**
 * exportNode の結果を `data/discussions/<workspace>/<type-dir>/<id>.md` に書き出す。
 * Phase 0 は depth=0 (隣接ノードは生成しない、 wikilink だけ埋める)。
 */

import fs from "node:fs";
import path from "node:path";

import type { createCore } from "../core/index.js";

import { exportNode, type ExportResult } from "./md-exporter.js";
import { TYPE_TO_DIR, type NodeType } from "./wikilink.js";

type Core = ReturnType<typeof createCore>;

export interface DumpOptions {
  /** 出力 root (default: ./data/discussions) */
  rootDir?: string;
  workspaceId: string;
}

export interface DumpResult extends ExportResult {
  filePath: string;
  written: "created" | "updated" | "unchanged";
}

export function dumpNode(
  core: Core,
  type: NodeType,
  id: string,
  options: DumpOptions
): DumpResult {
  const result = exportNode(core, type, options.workspaceId, id);
  const filePath = resolveFilePath(type, id, options);
  fs.mkdirSync(path.dirname(filePath), { recursive: true });

  let written: DumpResult["written"];
  const exists = fs.existsSync(filePath);
  if (exists) {
    const current = fs.readFileSync(filePath, "utf8");
    if (current === result.markdown) {
      written = "unchanged";
    } else {
      fs.writeFileSync(filePath, result.markdown);
      written = "updated";
    }
  } else {
    fs.writeFileSync(filePath, result.markdown);
    written = "created";
  }

  return { ...result, filePath, written };
}

export function resolveFilePath(
  type: NodeType,
  id: string,
  options: DumpOptions
): string {
  const root = options.rootDir ?? path.resolve("data/discussions");
  return path.join(root, options.workspaceId, TYPE_TO_DIR[type], `${id}.md`);
}
