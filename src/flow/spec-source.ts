/**
 * 仕様書ソース解決 (③ 仕様書を別途受け取って解析)。
 *
 * ② のインライン貼付 (Web textarea / Discord starter 本文) に加え、③ では仕様書を
 * **別チャネルで受け取る**: Web ファイルアップロード / Discord 添付ファイル / URL・パス指定。
 * いずれも最終的に**テキスト**へ解決し、② の `analyzeSpecMechanics` に渡す (解析は共通)。
 *
 * フォーマットは現状**テキスト系のみ** (md/txt/json/code 等、UTF-8 で読めるもの)。
 * バイナリ (PDF/docx/画像) は検出して弾く (テキスト抽出は後続)。サイズ上限あり。
 *
 * - Web (loopback 信頼) はローカルパス読みを許可する。
 * - Discord はサーバのローカルファイルを読ませない (URL=添付のみ、allowLocalPath=false)。
 *   これは CLAUDE.md の「認証境界 = Discord」「パストラバーサルを与えない」方針に沿う。
 *
 * fetch / fs は注入境界 (テストで差し替え可)。DB 非依存・単体テスト可能。
 */

import nodeFs from "node:fs";
import type fsType from "node:fs";

/** 1 仕様書あたりの最大バイト数 (既定 1MB)。 */
export const DEFAULT_MAX_SPEC_BYTES = 1_000_000;

export interface ResolveSpecOpts {
  /** 最大バイト数 (既定 DEFAULT_MAX_SPEC_BYTES)。 */
  maxBytes?: number;
  /** ローカルパス読みを許可するか (Web=true / Discord=false)。 */
  allowLocalPath?: boolean;
  /** fetch 実装 (既定 global fetch)。 */
  fetchImpl?: typeof fetch;
  /** fs (既定 node:fs。readFileSync / existsSync / statSync を使う)。 */
  fs?: Pick<typeof fsType, "readFileSync" | "existsSync" | "statSync">;
}

/** テキストとして扱えるか判定する (NUL バイトや非テキスト比率で弾く)。 */
export function isTextual(bytes: Uint8Array): boolean {
  if (bytes.length === 0) return false;
  const sample = bytes.subarray(0, Math.min(bytes.length, 4096));
  let suspicious = 0;
  for (const b of sample) {
    if (b === 0) return false; // NUL → バイナリ確定
    // 制御文字 (タブ/改行/復帰 を除く) を非テキストとして数える
    if (b < 0x09 || (b > 0x0d && b < 0x20)) suspicious++;
  }
  return suspicious / sample.length < 0.1;
}

function decodeTextual(bytes: Uint8Array, label: string): string {
  if (!isTextual(bytes)) {
    throw new Error(`${label} はテキストとして読めません (バイナリ形式の可能性。PDF/docx 等は未対応)`);
  }
  return new TextDecoder("utf-8", { fatal: false }).decode(bytes);
}

/** URL から仕様書テキストを取得する (text 系のみ・サイズ上限)。失敗は throw。 */
export async function fetchSpecText(url: string, opts: ResolveSpecOpts = {}): Promise<string> {
  const maxBytes = opts.maxBytes ?? DEFAULT_MAX_SPEC_BYTES;
  const doFetch = opts.fetchImpl ?? fetch;
  const res = await doFetch(url);
  if (!res.ok) throw new Error(`仕様書 URL の取得に失敗 (HTTP ${res.status}): ${url}`);
  const buf = new Uint8Array(await res.arrayBuffer());
  if (buf.length > maxBytes) {
    throw new Error(`仕様書が大きすぎます (${buf.length} > ${maxBytes} bytes): ${url}`);
  }
  return decodeTextual(buf, `URL ${url}`);
}

/** ローカルパスから仕様書テキストを読む (text 系のみ・サイズ上限)。失敗は throw。 */
export function readSpecTextFromFile(path: string, opts: ResolveSpecOpts = {}): string {
  const maxBytes = opts.maxBytes ?? DEFAULT_MAX_SPEC_BYTES;
  const fs = opts.fs ?? nodeFs;
  if (!fs.existsSync(path)) throw new Error(`仕様書ファイルが見つかりません: ${path}`);
  const size = fs.statSync(path).size;
  if (size > maxBytes) throw new Error(`仕様書が大きすぎます (${size} > ${maxBytes} bytes): ${path}`);
  const buf = fs.readFileSync(path);
  return decodeTextual(new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength), `ファイル ${path}`);
}

/** http(s) で始まれば URL、そうでなければローカルパスとして扱う。 */
export function isHttpUrl(s: string): boolean {
  return /^https?:\/\//i.test(s.trim());
}

/**
 * URL またはローカルパスを仕様書テキストに解決する。
 * http(s) → fetch、それ以外 → ローカルパス読み (allowLocalPath=false なら拒否)。
 */
export async function resolveSpecText(urlOrPath: string, opts: ResolveSpecOpts = {}): Promise<string> {
  const src = urlOrPath.trim();
  if (!src) throw new Error("仕様書ソースが空です");
  if (isHttpUrl(src)) return fetchSpecText(src, opts);
  if (!opts.allowLocalPath) {
    throw new Error("この経路ではローカルパスからの読み込みは許可されていません (URL を指定してください)");
  }
  return readSpecTextFromFile(src, opts);
}
