/**
 * 仕様書ソース解決 (③ 仕様書を別途受け取って解析)。
 *
 * ② のインライン貼付 (Web textarea / Discord starter 本文) に加え、③ では仕様書を
 * **別チャネルで受け取る**: Web ファイルアップロード / Discord 添付ファイル / URL・パス指定。
 * いずれも最終的に**テキスト**へ解決し、② の `analyzeSpecMechanics` に渡す (解析は共通)。
 *
 * フォーマット: テキスト系 (md/txt/json/code 等) + PDF (.pdf) + DOCX (.docx)。
 * PDF は pdf-parse、DOCX は mammoth で本文を抽出する (createRequire で CJS モジュールを読込)。
 * 画像等それ以外のバイナリは検出して弾く。サイズ上限あり。
 *
 * - Web (loopback 信頼) はローカルパス読みを許可する。
 * - Discord はサーバのローカルファイルを読ませない (URL=添付のみ、allowLocalPath=false)。
 *   これは CLAUDE.md の「認証境界 = Discord」「パストラバーサルを与えない」方針に沿う。
 *
 * fetch / fs は注入境界 (テストで差し替え可)。DB 非依存・単体テスト可能。
 * pdf-parse / mammoth の注入境界は PdfExtractor / DocxExtractor 型で差し替え可。
 */

import nodeFs from "node:fs";
import type fsType from "node:fs";
import { createRequire } from "node:module";
import nodePath from "node:path";

/** 1 仕様書あたりの最大バイト数 (既定 1MB)。 */
export const DEFAULT_MAX_SPEC_BYTES = 1_000_000;

/** PDF 抽出関数の型 (テスト注入用)。 */
export type PdfExtractor = (buf: Buffer) => Promise<{ text: string }>;
/** DOCX 抽出関数の型 (テスト注入用)。 */
export type DocxExtractor = (opts: { buffer: Buffer }) => Promise<{ value: string }>;

export interface ResolveSpecOpts {
  /** 最大バイト数 (既定 DEFAULT_MAX_SPEC_BYTES)。 */
  maxBytes?: number;
  /** ローカルパス読みを許可するか (Web=true / Discord=false)。 */
  allowLocalPath?: boolean;
  /** fetch 実装 (既定 global fetch)。 */
  fetchImpl?: typeof fetch;
  /** fs (既定 node:fs。readFileSync / existsSync / statSync を使う)。 */
  fs?: Pick<typeof fsType, "readFileSync" | "existsSync" | "statSync">;
  /** PDF 抽出実装 (既定 pdf-parse)。テスト用に差し替え可。 */
  pdfExtractor?: PdfExtractor;
  /** DOCX 抽出実装 (既定 mammoth)。テスト用に差し替え可。 */
  docxExtractor?: DocxExtractor;
}

// ── バイナリ形式の検出 ──────────────────────────────────────────────────────

/** マジックバイトで PDF かどうか判定する (%PDF-)。 */
export function isPdfBytes(bytes: Uint8Array): boolean {
  return bytes[0] === 0x25 && bytes[1] === 0x50 && bytes[2] === 0x44 && bytes[3] === 0x46;
}

/** マジックバイトで DOCX/ZIP かどうか判定する (PK ヘッダ 0x50 0x4B 0x03 0x04)。 */
export function isZipBytes(bytes: Uint8Array): boolean {
  return bytes[0] === 0x50 && bytes[1] === 0x4b && bytes[2] === 0x03 && bytes[3] === 0x04;
}

/** 拡張子で PDF かどうか判定する。 */
function isPdfExt(filePath: string): boolean {
  return nodePath.extname(filePath).toLowerCase() === ".pdf";
}

/** 拡張子で DOCX かどうか判定する。 */
function isDocxExt(filePath: string): boolean {
  return nodePath.extname(filePath).toLowerCase() === ".docx";
}

// ── PDF / DOCX 抽出 ─────────────────────────────────────────────────────────

/** 実 pdf-parse をロードする (createRequire で CJS 読込)。 */
function loadPdfParse(): PdfExtractor {
  const r = createRequire(import.meta.url);
  const mod = r("pdf-parse") as { default?: PdfExtractor } | PdfExtractor;
  if (typeof mod === "function") return mod;
  if (mod && typeof (mod as { default?: PdfExtractor }).default === "function") {
    return (mod as { default: PdfExtractor }).default;
  }
  throw new Error("pdf-parse の読み込みに失敗しました");
}

/** 実 mammoth をロードする (createRequire で CJS 読込)。 */
function loadMammoth(): DocxExtractor {
  const r = createRequire(import.meta.url);
  const mod = r("mammoth") as { extractRawText?: DocxExtractor };
  if (typeof mod.extractRawText !== "function") {
    throw new Error("mammoth.extractRawText の読み込みに失敗しました");
  }
  return mod.extractRawText.bind(mod);
}

/** PDF バイトから本文テキストを抽出する。失敗は throw。 */
async function extractPdf(bytes: Uint8Array, label: string, extractor?: PdfExtractor): Promise<string> {
  const parse = extractor ?? loadPdfParse();
  const buf = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const result = await parse(buf);
  const text = result.text?.trim() ?? "";
  if (!text) throw new Error(`${label}: PDF からテキストを抽出できませんでした (本文が空)`);
  return text;
}

/** DOCX バイトから本文テキストを抽出する。失敗は throw。 */
async function extractDocx(bytes: Uint8Array, label: string, extractor?: DocxExtractor): Promise<string> {
  const mammoth = extractor ?? loadMammoth();
  const buf = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const result = await mammoth({ buffer: buf });
  const text = result.value?.trim() ?? "";
  if (!text) throw new Error(`${label}: DOCX からテキストを抽出できませんでした (本文が空)`);
  return text;
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
    throw new Error(`${label} はテキストとして読めません (バイナリ形式の可能性。PDF は .pdf, DOCX は .docx を使用してください)`);
  }
  return new TextDecoder("utf-8", { fatal: false }).decode(bytes);
}

/**
 * URL から仕様書テキストを取得する。
 * PDF (Content-Type application/pdf または .pdf URL) / DOCX (.docx URL) は専用パーサで抽出。
 * それ以外はテキストとして読む (非テキストは throw)。
 */
export async function fetchSpecText(url: string, opts: ResolveSpecOpts = {}): Promise<string> {
  const maxBytes = opts.maxBytes ?? DEFAULT_MAX_SPEC_BYTES;
  const doFetch = opts.fetchImpl ?? fetch;
  const res = await doFetch(url);
  if (!res.ok) throw new Error(`仕様書 URL の取得に失敗 (HTTP ${res.status}): ${url}`);
  const buf = new Uint8Array(await res.arrayBuffer());
  if (buf.length > maxBytes) {
    throw new Error(`仕様書が大きすぎます (${buf.length} > ${maxBytes} bytes): ${url}`);
  }
  const contentType = ((res as unknown as { headers?: { get?: (k: string) => string | null } }).headers?.get?.("content-type") ?? "").toLowerCase();
  const urlPath = (() => { try { return new URL(url).pathname; } catch { return url; } })();
  if (contentType.includes("application/pdf") || isPdfExt(urlPath)) {
    return extractPdf(buf, `URL ${url}`, opts.pdfExtractor);
  }
  if (contentType.includes("officedocument.wordprocessingml") || isDocxExt(urlPath)) {
    return extractDocx(buf, `URL ${url}`, opts.docxExtractor);
  }
  return decodeTextual(buf, `URL ${url}`);
}

/**
 * ローカルパスから仕様書テキストを読む。
 * .pdf → pdf-parse、.docx → mammoth でテキスト抽出。それ以外はテキスト読み (バイナリは throw)。
 */
export async function readSpecTextFromFile(filePath: string, opts: ResolveSpecOpts = {}): Promise<string> {
  const maxBytes = opts.maxBytes ?? DEFAULT_MAX_SPEC_BYTES;
  const fs = opts.fs ?? nodeFs;
  if (!fs.existsSync(filePath)) throw new Error(`仕様書ファイルが見つかりません: ${filePath}`);
  const size = fs.statSync(filePath).size;
  if (size > maxBytes) throw new Error(`仕様書が大きすぎます (${size} > ${maxBytes} bytes): ${filePath}`);
  const raw = fs.readFileSync(filePath);
  const buf = new Uint8Array(raw.buffer, raw.byteOffset, raw.byteLength);
  if (isPdfExt(filePath) || isPdfBytes(buf)) {
    return extractPdf(buf, `ファイル ${filePath}`, opts.pdfExtractor);
  }
  if (isDocxExt(filePath) || isZipBytes(buf)) {
    return extractDocx(buf, `ファイル ${filePath}`, opts.docxExtractor);
  }
  return decodeTextual(buf, `ファイル ${filePath}`);
}

/** http(s) で始まれば URL、そうでなければローカルパスとして扱う。 */
export function isHttpUrl(s: string): boolean {
  return /^https?:\/\//i.test(s.trim());
}

/**
 * URL またはローカルパスを仕様書テキストに解決する。
 * http(s) → fetch、それ以外 → ローカルパス読み (allowLocalPath=false なら拒否)。
 * PDF / DOCX はパーサで本文を抽出してから返す。
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

/**
 * バイト列から仕様書テキストを抽出する (Web ファイルアップロード用)。
 * PDF / DOCX / テキスト系を自動判別。バイナリ (画像等) は throw。
 */
export async function extractSpecTextFromBytes(
  bytes: Uint8Array,
  fileName: string,
  opts: Pick<ResolveSpecOpts, "pdfExtractor" | "docxExtractor"> = {}
): Promise<string> {
  const label = `ファイル ${fileName}`;
  if (isPdfExt(fileName) || isPdfBytes(bytes)) return extractPdf(bytes, label, opts.pdfExtractor);
  if (isDocxExt(fileName) || isZipBytes(bytes)) return extractDocx(bytes, label, opts.docxExtractor);
  return decodeTextual(bytes, label);
}
