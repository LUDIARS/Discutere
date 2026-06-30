/**
 * Anatomia CLI アクセス (I/O 境界)。
 *
 * `node <binPath> domains list --json` を spawn してドメインカードを取得する。
 * カードが無ければ (`autoDraft`) `domains draft` で下地を作ってから再取得する。
 *
 * SRP: Anatomia CLI の起動と JSON パースのみ。精製 (→メカニクス) は `refine.ts`。
 * 取得経路は HTTP ではなく CLI: Anatomia web server の常駐を要求せず、`anatomia-analyze`
 * skill と同じ経路 (= 既存運用と一致) で疎結合に取れるため。
 */

import { execFile } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import type { AnatomiaDomainCard, AnatomiaDomainsResponse, AnatomiaSource } from "./types.js";

/** CLI 実行ランナー (テストで差し替え可能にするための DI 境界)。 */
export interface AnatomiaCliRunner {
  /**
   * binPath を第 1 引数に、残りを CLI サブコマンド引数として実行する。
   * cwd は Anatomia home (`<cwd>/.anatomia/projects.json` をプロジェクト登録簿として解決する) になる。
   */
  run(
    binPath: string,
    args: string[],
    timeoutMs: number,
    cwd?: string
  ): Promise<{ stdout: string; exitCode: number }>;
}

/** 既定ランナー: shell を介さず引数配列で execFile (インジェクション回避)。 */
export const defaultAnatomiaCliRunner: AnatomiaCliRunner = {
  run(binPath, args, timeoutMs, cwd) {
    return new Promise((resolve, reject) => {
      const child = execFile(
        process.execPath,
        [binPath, ...args],
        { cwd, timeout: timeoutMs, maxBuffer: 32 * 1024 * 1024, encoding: "utf8", windowsHide: true },
        (err, stdout, stderr) => {
          // execFile は exit!=0 / timeout を err にする。exitCode を取り出して呼び出し側で判定。
          if (err && (err as NodeJS.ErrnoException).code === "ENOENT") {
            reject(new Error(`Anatomia CLI を起動できません (${process.execPath}): ${err.message}`));
            return;
          }
          const exitCode = typeof (err as { code?: unknown } | null)?.code === "number" ? (err as { code: number }).code : err ? 1 : 0;
          if (err && exitCode === 0) {
            // timeout 等 (code が文字列) は失敗として扱う。
            reject(new Error(`Anatomia CLI 実行失敗: ${err.message}${stderr ? ` / ${stderr}` : ""}`));
            return;
          }
          resolve({ stdout, exitCode });
        }
      );
      child.on("error", reject);
    });
  },
};

export interface FetchAnatomiaDomainsOpts {
  /** Anatomia bin/anatomia.mjs の絶対パス。空なら sibling リポを解決する。 */
  binPath?: string;
  /** domains list が空のとき domains draft を自動実行するか。既定 true。 */
  autoDraft?: boolean;
  /** CLI 1 コマンドのタイムアウト (ms)。既定 180000。 */
  timeoutMs?: number;
  runner?: AnatomiaCliRunner;
  warn?: (msg: string) => void;
}

/** sibling リポ既定パス (`<cwd>/../Anatomia/bin/anatomia.mjs`) を解決する。 */
function defaultSiblingBin(): string {
  return path.resolve(process.cwd(), "..", "Anatomia", "bin", "anatomia.mjs");
}

/** source を CLI 引数に変換する (project 優先)。 */
function sourceArgs(source: AnatomiaSource): string[] {
  if ("project" in source) {
    if (!source.project.trim()) throw new Error("Anatomia source.project が空です");
    return ["--project", source.project];
  }
  if (!source.repoPath.trim()) throw new Error("Anatomia source.repoPath が空です");
  return ["--repo", source.repoPath];
}

function parseDomainsJson(stdout: string): AnatomiaDomainsResponse {
  let parsed: unknown;
  try {
    parsed = JSON.parse(stdout);
  } catch (e) {
    throw new Error(`Anatomia domains 出力が JSON ではありません: ${(e as Error).message}`);
  }
  const obj = parsed as { dir?: unknown; domains?: unknown };
  if (!obj || typeof obj !== "object" || !Array.isArray(obj.domains)) {
    throw new Error("Anatomia domains 出力に domains 配列がありません");
  }
  const domains: AnatomiaDomainCard[] = obj.domains
    .map((d): AnatomiaDomainCard | null => {
      if (!d || typeof d !== "object") return null;
      const o = d as Record<string, unknown>;
      const name = typeof o.name === "string" ? o.name : "";
      if (!name) return null;
      return {
        name,
        description: typeof o.description === "string" ? o.description : "",
        mechanics: Array.isArray(o.mechanics) ? o.mechanics.filter((m): m is string => typeof m === "string") : [],
        specRefs: Array.isArray(o.specRefs) ? o.specRefs.filter((s): s is string => typeof s === "string") : [],
        rationale: typeof o.rationale === "string" ? o.rationale : undefined,
      };
    })
    .filter((d): d is AnatomiaDomainCard => d !== null);
  return { dir: typeof obj.dir === "string" ? obj.dir : "", domains };
}

/**
 * Anatomia からドメインカードを取得する。
 *
 * fail-fast (§7.1): bin が見つからない / CLI が非 0 終了 / JSON 不正は throw する
 * (黙ってスタブ/空配列に逃げない)。autoDraft で下地を作っても空なら空配列を返す
 * (= 解析対象に下地が無いという正当な結果。これは degrade ではなく事実)。
 */
export async function fetchAnatomiaDomains(
  source: AnatomiaSource,
  opts: FetchAnatomiaDomainsOpts = {}
): Promise<AnatomiaDomainCard[]> {
  const runner = opts.runner ?? defaultAnatomiaCliRunner;
  const timeoutMs = opts.timeoutMs ?? 180000;
  const autoDraft = opts.autoDraft ?? true;
  const warn = opts.warn ?? (() => {});

  const binPath = opts.binPath?.trim() ? opts.binPath.trim() : defaultSiblingBin();
  if (!fs.existsSync(binPath)) {
    throw new Error(
      `Anatomia CLI が見つかりません: ${binPath} (flow.anatomia.binPath / DISCUTERE_ANATOMIA_BIN を設定してください)`
    );
  }

  const src = sourceArgs(source);
  // Anatomia home はプロジェクト登録簿 (`<home>/.anatomia/projects.json`) の場所。--project 解決に必要。
  // binPath = <anatomiaRoot>/bin/anatomia.mjs なので、その root を cwd にすると登録簿が見つかる
  // (anatomia-analyze skill が `project add` する既定の home と一致)。
  const anatomiaHome = path.dirname(path.dirname(binPath));

  const list = async (): Promise<AnatomiaDomainCard[]> => {
    const { stdout, exitCode } = await runner.run(binPath, ["domains", "list", ...src, "--json"], timeoutMs, anatomiaHome);
    if (exitCode !== 0) throw new Error(`anatomia domains list 失敗 (exit ${exitCode})`);
    return parseDomainsJson(stdout).domains;
  };

  let domains = await list();
  if (domains.length === 0 && autoDraft) {
    warn("Anatomia: ドメイン下地が無いため domains draft を実行します (初回は解析で時間がかかります)");
    const { exitCode } = await runner.run(binPath, ["domains", "draft", ...src, "--json"], timeoutMs, anatomiaHome);
    if (exitCode !== 0) throw new Error(`anatomia domains draft 失敗 (exit ${exitCode})`);
    domains = await list();
  }
  return domains;
}
