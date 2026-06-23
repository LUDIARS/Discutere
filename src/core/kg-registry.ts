/**
 * タスク別 KG レジストリの解決 — spec/feature/crawler/EXTERNAL-SOURCES.md §12.
 *
 * 学習データ KG を「タスク (収集目的) 別」に複数宣言し、 起動時に 1 つを active として開く。
 * Core を開く全箇所はここの `resolveActiveKgPath` を経由して active KG を見る (SRP / 散在解消)。
 *
 * 解決順 (完全後方互換):
 *   1. knowledgeGraphs があれば activeKg (既定 "default") に一致する id の kuzuPath。
 *      一致が無く list が非空なら先頭にフォールバック。
 *   2. 無ければ旧 `kuzuPath` 単独 (= id="default" の 1 KG とみなす)。
 *   3. どちらも無ければ undefined (KuzuClient の adapter 既定に倒す)。
 *
 * 無停止ホットスワップは非対応 (§10)。 切替は config の activeKg 変更 + 再起動。
 */

import path from "node:path";

import type { DiscutereConfig } from "../config.js";

const DEFAULT_KG_ID = "default";

export interface KnowledgeGraphEntry {
  id: string;
  label: string;
  kuzuPath: string;
  active: boolean;
}

/** active KG の kuzu パスを解決する。 該当が無ければ undefined (adapter 既定)。 */
export function resolveActiveKgPath(config: DiscutereConfig): string | undefined {
  const graphs = config.discatier.knowledgeGraphs;
  if (graphs && graphs.length > 0) {
    const activeId = config.discatier.activeKg ?? DEFAULT_KG_ID;
    const match = graphs.find((kg) => kg.id === activeId);
    return (match ?? graphs[0]).kuzuPath;
  }
  // 旧 単一 kuzuPath (それも無ければ undefined = adapter 既定)。
  return config.discatier.kuzuPath;
}

/**
 * sidecar (.ingested / .raw / .attribution) を置く active KG のディレクトリ。
 * KG を切り替えれば dedup 履歴・source 帰属も一緒に切り替わる (データ隔離が成立、§12)。
 * active KG パスが未解決のときは従来どおり ./data/external に倒す。
 */
export function resolveActiveKgDir(config: DiscutereConfig): string {
  const kgPath = resolveActiveKgPath(config);
  if (!kgPath) return path.resolve("./data/external");
  return path.dirname(path.resolve(kgPath));
}

/** 宣言済み KG 一覧 (active フラグ付き)。 旧 単一 kuzuPath は default 1 件として見せる。 */
export function listKnowledgeGraphs(config: DiscutereConfig): KnowledgeGraphEntry[] {
  const graphs = config.discatier.knowledgeGraphs;
  if (graphs && graphs.length > 0) {
    const activeId = config.discatier.activeKg ?? DEFAULT_KG_ID;
    const hasMatch = graphs.some((kg) => kg.id === activeId);
    return graphs.map((kg, i) => ({
      id: kg.id,
      label: kg.label ?? kg.id,
      kuzuPath: kg.kuzuPath,
      // 一致が無ければ先頭が実効 active (resolveActiveKgPath と整合)。
      active: hasMatch ? kg.id === activeId : i === 0,
    }));
  }
  if (config.discatier.kuzuPath) {
    return [{ id: DEFAULT_KG_ID, label: "総合", kuzuPath: config.discatier.kuzuPath, active: true }];
  }
  return [];
}
