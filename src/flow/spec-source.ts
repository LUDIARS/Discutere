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
import zlib from "node:zlib";

/** 1 仕様書あたりの最大バイト数 (既定 1MB)。 */
export const DEFAULT_MAX_SPEC_BYTES = 5_000_000;

export const SUPPORTED_SPEC_EXTENSIONS = [
  ".doc",
  ".docx",
  ".htm",
  ".html",
  ".json",
  ".md",
  ".markdown",
  ".pdf",
  ".pptx",
  ".text",
  ".txt",
  ".xlsx",
  ".xslx",
  ".yaml",
  ".yml",
] as const;

const SUPPORTED_SPEC_EXTS = new Set<string>(SUPPORTED_SPEC_EXTENSIONS);

function specExt(filePath: string): string {
  return nodePath.extname(filePath).toLowerCase();
}

export function isSupportedSpecFileName(fileName: string): boolean {
  return SUPPORTED_SPEC_EXTS.has(specExt(fileName));
}

function assertSupportedSpecFileName(fileName: string, label: string): void {
  if (!isSupportedSpecFileName(fileName)) {
    throw new Error(`${label}: unsupported spec file format. Supported: ${SUPPORTED_SPEC_EXTENSIONS.join(", ")}`);
  }
}

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
  githubToken?: string | null;
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
  return specExt(filePath) === ".pdf";
}

/** 拡張子で DOCX かどうか判定する。 */
function isDocxExt(filePath: string): boolean {
  return specExt(filePath) === ".docx";
}

function isDocExt(filePath: string): boolean {
  return specExt(filePath) === ".doc";
}

function isPptxExt(filePath: string): boolean {
  return specExt(filePath) === ".pptx";
}

function isXlsxExt(filePath: string): boolean {
  const ext = specExt(filePath);
  return ext === ".xlsx" || ext === ".xslx";
}

function isHtmlExt(filePath: string): boolean {
  const ext = specExt(filePath);
  return ext === ".html" || ext === ".htm";
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
function readU16(bytes: Uint8Array, offset: number): number {
  return bytes[offset]! | (bytes[offset + 1]! << 8);
}

function readU32(bytes: Uint8Array, offset: number): number {
  return (bytes[offset]! | (bytes[offset + 1]! << 8) | (bytes[offset + 2]! << 16) | (bytes[offset + 3]! << 24)) >>> 0;
}

function findZipEocd(bytes: Uint8Array): number {
  const min = Math.max(0, bytes.length - 65557);
  for (let i = bytes.length - 22; i >= min; i--) {
    if (readU32(bytes, i) === 0x06054b50) return i;
  }
  return -1;
}

function readZipTextEntries(bytes: Uint8Array, label: string): Map<string, string> {
  const eocd = findZipEocd(bytes);
  if (eocd < 0) throw new Error(`${label}: ZIP central directory was not found`);
  const entries = readU16(bytes, eocd + 10);
  const cdOffset = readU32(bytes, eocd + 16);
  const out = new Map<string, string>();
  let off = cdOffset;
  const decoder = new TextDecoder("utf-8", { fatal: false });
  for (let i = 0; i < entries; i++) {
    if (readU32(bytes, off) !== 0x02014b50) throw new Error(`${label}: invalid ZIP central directory`);
    const compression = readU16(bytes, off + 10);
    const compressedSize = readU32(bytes, off + 20);
    const nameLen = readU16(bytes, off + 28);
    const extraLen = readU16(bytes, off + 30);
    const commentLen = readU16(bytes, off + 32);
    const localOffset = readU32(bytes, off + 42);
    const name = decoder.decode(bytes.subarray(off + 46, off + 46 + nameLen));
    off += 46 + nameLen + extraLen + commentLen;

    if (readU32(bytes, localOffset) !== 0x04034b50) continue;
    const localNameLen = readU16(bytes, localOffset + 26);
    const localExtraLen = readU16(bytes, localOffset + 28);
    const dataStart = localOffset + 30 + localNameLen + localExtraLen;
    const data = Buffer.from(bytes.subarray(dataStart, dataStart + compressedSize));
    let raw: Buffer;
    if (compression === 0) raw = data;
    else if (compression === 8) raw = zlib.inflateRawSync(data);
    else continue;
    out.set(name, raw.toString("utf8"));
  }
  return out;
}

function decodeXmlEntities(s: string): string {
  return s.replace(/&(#x[0-9a-f]+|#\d+|amp|lt|gt|quot|apos|nbsp);/gi, (_m, ent: string) => {
    const e = ent.toLowerCase();
    if (e === "amp") return "&";
    if (e === "lt") return "<";
    if (e === "gt") return ">";
    if (e === "quot") return '"';
    if (e === "apos") return "'";
    if (e === "nbsp") return " ";
    if (e.startsWith("#x")) return String.fromCodePoint(Number.parseInt(e.slice(2), 16));
    if (e.startsWith("#")) return String.fromCodePoint(Number.parseInt(e.slice(1), 10));
    return "";
  });
}

function xmlToPlainText(xml: string): string {
  return decodeXmlEntities(xml.replace(/<[^>]+>/g, " "))
    .replace(/\s+/g, " ")
    .trim();
}

function extractPptx(bytes: Uint8Array, label: string): string {
  const zip = readZipTextEntries(bytes, label);
  const parts = [...zip.entries()]
    .filter(([name]) => /^ppt\/slides\/slide\d+\.xml$/i.test(name) || /^ppt\/notesSlides\/notesSlide\d+\.xml$/i.test(name))
    .sort(([a], [b]) => a.localeCompare(b))
    .map(([, xml]) => xmlToPlainText(xml))
    .filter(Boolean);
  if (parts.length === 0) throw new Error(`${label}: PPTX text was not found`);
  return parts.join("\n\n");
}

function extractXlsx(bytes: Uint8Array, label: string): string {
  const zip = readZipTextEntries(bytes, label);
  const parts = [...zip.entries()]
    .filter(([name]) => /^xl\/sharedStrings\.xml$/i.test(name) || /^xl\/worksheets\/sheet\d+\.xml$/i.test(name))
    .sort(([a], [b]) => a.localeCompare(b))
    .map(([, xml]) => xmlToPlainText(xml))
    .filter(Boolean);
  if (parts.length === 0) throw new Error(`${label}: XLSX text was not found`);
  return parts.join("\n\n");
}

function stripHtml(html: string): string {
  return decodeXmlEntities(
    html
      .replace(/<script\b[^>]*>[\s\S]*?<\/script>/gi, " ")
      .replace(/<style\b[^>]*>[\s\S]*?<\/style>/gi, " ")
      .replace(/<[^>]+>/g, " ")
  )
    .replace(/\s+/g, " ")
    .trim();
}

function extractLegacyDoc(bytes: Uint8Array, label: string): string {
  const ascii = Buffer.from(bytes)
    .toString("latin1")
    .match(/[ -~\t\r\n]{4,}/g)
    ?.join(" ") ?? "";
  const utf16: string[] = [];
  let run = "";
  for (let i = 0; i + 1 < bytes.length; i += 2) {
    const code = bytes[i]! | (bytes[i + 1]! << 8);
    if (code === 0x09 || code === 0x0a || code === 0x0d || (code >= 0x20 && code <= 0xd7ff)) {
      run += String.fromCharCode(code);
    } else {
      if (run.trim().length >= 4) utf16.push(run.trim());
      run = "";
    }
  }
  if (run.trim().length >= 4) utf16.push(run.trim());
  const text = (utf16.join(" ").length > ascii.length ? utf16.join(" ") : ascii).replace(/\s+/g, " ").trim();
  if (text.length < 10) throw new Error(`${label}: legacy DOC text could not be extracted. Convert to DOCX if possible.`);
  return text;
}

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

export interface GithubSpecSource {
  owner: string;
  repo: string;
  path?: string;
  ref?: string;
}

export interface GithubSpecTextResult {
  source: GithubSpecSource & { path: string };
  text: string;
  size: number;
  sha?: string;
  htmlUrl?: string;
}

function stripGitSuffix(repo: string): string {
  return repo.replace(/\.git$/i, "");
}

function parseGithubSshUrl(input: string): GithubSpecSource | null {
  const m = /^git@github\.com:([^/\s]+)\/([^/\s]+?)(?:\.git)?$/i.exec(input.trim());
  return m ? { owner: m[1]!, repo: stripGitSuffix(m[2]!) } : null;
}

export function parseGithubSpecUrl(input: string): GithubSpecSource | null {
  const ssh = parseGithubSshUrl(input);
  if (ssh) return ssh;
  let u: URL;
  try {
    u = new URL(input.trim());
  } catch {
    return null;
  }
  const host = u.hostname.toLowerCase();
  const parts = u.pathname.split("/").filter(Boolean);
  if (host === "raw.githubusercontent.com" && parts.length >= 4) {
    return { owner: parts[0]!, repo: stripGitSuffix(parts[1]!), ref: parts[2]!, path: parts.slice(3).join("/") };
  }
  if (host === "api.github.com" && parts[0] === "repos" && parts.length >= 4) {
    const contentIndex = parts.indexOf("contents");
    return {
      owner: parts[1]!,
      repo: stripGitSuffix(parts[2]!),
      path: contentIndex >= 0 ? parts.slice(contentIndex + 1).join("/") : undefined,
      ref: u.searchParams.get("ref") || undefined,
    };
  }
  if (host !== "github.com" || parts.length < 2) return null;
  const out: GithubSpecSource = { owner: parts[0]!, repo: stripGitSuffix(parts[1]!) };
  const mode = parts[2];
  if ((mode === "blob" || mode === "raw") && parts.length >= 5) {
    out.ref = parts[3]!;
    out.path = parts.slice(4).join("/");
  } else if (mode === "tree" && parts.length >= 4) {
    out.ref = parts[3]!;
    out.path = parts.slice(4).join("/") || undefined;
  }
  out.ref = u.searchParams.get("ref") || out.ref;
  out.path = u.searchParams.get("path") || out.path;
  return out;
}

export function buildGithubSpecSource(repoUrl: string, path?: string, ref?: string): GithubSpecSource | null {
  const parsed = parseGithubSpecUrl(repoUrl);
  if (!parsed) return null;
  return {
    ...parsed,
    path: path?.trim() || parsed.path,
    ref: ref?.trim() || parsed.ref,
  };
}

function githubHeaders(token?: string | null): HeadersInit {
  return {
    accept: "application/vnd.github+json",
    "user-agent": "LUDIARS-Discutere",
    ...(token ? { authorization: `Bearer ${token}` } : {}),
  };
}

function encodeGithubPath(path: string): string {
  return path.split("/").map(encodeURIComponent).join("/");
}

export async function fetchGithubRepoInfo(
  source: Pick<GithubSpecSource, "owner" | "repo">,
  opts: Pick<ResolveSpecOpts, "fetchImpl" | "githubToken"> = {}
): Promise<{ owner: string; repo: string; private?: boolean; defaultBranch?: string; htmlUrl?: string }> {
  const doFetch = opts.fetchImpl ?? fetch;
  const res = await doFetch(`https://api.github.com/repos/${source.owner}/${source.repo}`, {
    headers: githubHeaders(opts.githubToken),
  });
  if (!res.ok) throw new Error(`GitHub repository read failed (HTTP ${res.status}): ${source.owner}/${source.repo}`);
  const data = (await res.json()) as Record<string, unknown>;
  return {
    owner: source.owner,
    repo: source.repo,
    private: typeof data.private === "boolean" ? data.private : undefined,
    defaultBranch: typeof data.default_branch === "string" ? data.default_branch : undefined,
    htmlUrl: typeof data.html_url === "string" ? data.html_url : undefined,
  };
}

export async function fetchGithubSpecText(
  source: GithubSpecSource,
  opts: ResolveSpecOpts = {}
): Promise<GithubSpecTextResult> {
  if (!source.path) throw new Error("GitHub URL requires a file path (blob URL or path field)");
  assertSupportedSpecFileName(source.path, `GitHub ${source.owner}/${source.repo}/${source.path}`);
  const maxBytes = opts.maxBytes ?? DEFAULT_MAX_SPEC_BYTES;
  const doFetch = opts.fetchImpl ?? fetch;
  const query = source.ref ? `?ref=${encodeURIComponent(source.ref)}` : "";
  const url = `https://api.github.com/repos/${source.owner}/${source.repo}/contents/${encodeGithubPath(source.path)}${query}`;
  const res = await doFetch(url, { headers: githubHeaders(opts.githubToken) });
  if (!res.ok) throw new Error(`GitHub file read failed (HTTP ${res.status}): ${source.owner}/${source.repo}/${source.path}`);
  const data = (await res.json()) as Record<string, unknown>;
  if (Array.isArray(data)) throw new Error(`GitHub path is a directory, not a file: ${source.path}`);
  if (data.type !== "file") throw new Error(`GitHub path is not a file: ${source.path}`);
  const size = typeof data.size === "number" ? data.size : 0;
  if (size > maxBytes) throw new Error(`GitHub file is too large (${size} > ${maxBytes} bytes): ${source.path}`);
  const content = typeof data.content === "string" ? data.content : "";
  const encoding = typeof data.encoding === "string" ? data.encoding : "";
  if (encoding !== "base64" || !content) throw new Error(`GitHub contents API did not return base64 file content: ${source.path}`);
  const bytes = new Uint8Array(Buffer.from(content.replace(/\s+/g, ""), "base64"));
  const text = await extractSpecTextFromBytes(bytes, source.path, opts);
  return {
    source: { ...source, path: source.path },
    text,
    size,
    sha: typeof data.sha === "string" ? data.sha : undefined,
    htmlUrl: typeof data.html_url === "string" ? data.html_url : undefined,
  };
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
  const ext = specExt(urlPath);
  if (ext && !SUPPORTED_SPEC_EXTS.has(ext)) assertSupportedSpecFileName(urlPath, `URL ${url}`);
  if (contentType.includes("application/pdf") || isPdfExt(urlPath)) {
    return extractPdf(buf, `URL ${url}`, opts.pdfExtractor);
  }
  if (contentType.includes("officedocument.wordprocessingml") || isDocxExt(urlPath)) {
    return extractDocx(buf, `URL ${url}`, opts.docxExtractor);
  }
  if (contentType.includes("presentationml") || isPptxExt(urlPath)) return extractPptx(buf, `URL ${url}`);
  if (contentType.includes("spreadsheetml") || isXlsxExt(urlPath)) return extractXlsx(buf, `URL ${url}`);
  if (isDocExt(urlPath)) return extractLegacyDoc(buf, `URL ${url}`);
  const decoded = decodeTextual(buf, `URL ${url}`);
  return contentType.includes("text/html") || isHtmlExt(urlPath) ? stripHtml(decoded) : decoded;
}

/**
 * ローカルパスから仕様書テキストを読む。
 * .pdf → pdf-parse、.docx → mammoth でテキスト抽出。それ以外はテキスト読み (バイナリは throw)。
 */
export async function readSpecTextFromFile(filePath: string, opts: ResolveSpecOpts = {}): Promise<string> {
  assertSupportedSpecFileName(filePath, `ファイル ${filePath}`);
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
  if (isHtmlExt(filePath)) return stripHtml(decodeTextual(buf, `ファイル ${filePath}`));
  if (isDocExt(filePath)) return extractLegacyDoc(buf, `ファイル ${filePath}`);
  if (isDocxExt(filePath) || isZipBytes(buf)) {
    if (isPptxExt(filePath)) return extractPptx(buf, `ファイル ${filePath}`);
    if (isXlsxExt(filePath)) return extractXlsx(buf, `ファイル ${filePath}`);
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
  const github = parseGithubSpecUrl(src);
  if (github) return (await fetchGithubSpecText(github, opts)).text;
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
  assertSupportedSpecFileName(fileName, label);
  if (isPdfExt(fileName) || isPdfBytes(bytes)) return extractPdf(bytes, label, opts.pdfExtractor);
  if (isPptxExt(fileName)) return extractPptx(bytes, label);
  if (isXlsxExt(fileName)) return extractXlsx(bytes, label);
  if (isDocExt(fileName)) return extractLegacyDoc(bytes, label);
  if (isHtmlExt(fileName)) return stripHtml(decodeTextual(bytes, label));
  if (isDocxExt(fileName) || isZipBytes(bytes)) return extractDocx(bytes, label, opts.docxExtractor);
  return decodeTextual(bytes, label);
}
