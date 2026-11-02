// 仕様書の抽出 (PDF / DOCX / PPTX / XLSX / 旧 DOC) を Node のまま行うサイドカー。
// C++ (native/src/flow/spec_source.cpp) が引数配列で起動し、標準入力に JSON を 1 つ渡す:
//   { "fileName": "spec.pdf" | "spec.docx" | "spec.pptx" | "spec.xlsx" | "spec.doc", "label": "URL ...", "base64": "..." }
// 移植前の src/flow/spec-source.ts の extractSpecTextFromBytes をそのまま呼ぶ (ZIP 展開の上限 PR #3487 もそのまま効く)。
// 標準出力には {"ok":true,"result":{"text":...}} か {"ok":false,"error":"..."} を 1 行だけ書く。
// ファイルパスは受け取らない (バイト列だけ)。
const MAX_INPUT_CHARS = Math.ceil(((64 * 1024 * 1024) * 4) / 3) + 4096;
const ALLOWED_NAMES = new Set(["spec.pdf", "spec.docx", "spec.pptx", "spec.xlsx", "spec.doc"]);

// 依存ライブラリの console 出力で標準出力の JSON を壊さないよう、全部標準エラーへ回す。
const toStderr = (...args: unknown[]) => process.stderr.write(`${args.map(String).join(" ")}\n`);
console.log = toStderr;
console.info = toStderr;
console.warn = toStderr;
console.debug = toStderr;

function reply(value: unknown): void {
  process.stdout.write(`${JSON.stringify(value)}\n`);
}

async function readStdin(): Promise<string> {
  const chunks: Buffer[] = [];
  let size = 0;
  for await (const chunk of process.stdin) {
    const buf = Buffer.isBuffer(chunk) ? chunk : Buffer.from(String(chunk));
    size += buf.length;
    if (size > MAX_INPUT_CHARS + 1024) throw new Error("sidecar input too large");
    chunks.push(buf);
  }
  return Buffer.concat(chunks).toString("utf8");
}

async function main(): Promise<void> {
  let input: { fileName?: unknown; label?: unknown; base64?: unknown };
  try {
    input = JSON.parse(await readStdin()) as typeof input;
  } catch (error) {
    reply({ ok: false, error: `invalid sidecar input: ${(error as Error).message}` });
    return;
  }
  const fileName = typeof input.fileName === "string" ? input.fileName : "";
  const label = typeof input.label === "string" ? input.label : "";
  const base64 = typeof input.base64 === "string" ? input.base64 : "";
  if (!ALLOWED_NAMES.has(fileName) || base64.length > MAX_INPUT_CHARS) {
    reply({ ok: false, error: "invalid sidecar input" });
    return;
  }
  const { extractSpecTextFromBytes } = await import("../../src/flow/spec-source.js");
  try {
    const bytes = new Uint8Array(Buffer.from(base64, "base64"));
    const text = await extractSpecTextFromBytes(bytes, fileName);
    reply({ ok: true, result: { text } });
  } catch (error) {
    // 抽出関数の文言は "ファイル <fileName>: ..."。呼び出し元の見出し (URL ... / ファイル <実パス>) に戻す。
    let message = error instanceof Error ? error.message : String(error);
    const prefix = `ファイル ${fileName}`;
    if (label && message.startsWith(prefix)) message = label + message.slice(prefix.length);
    reply({ ok: false, error: message });
  }
}

main().catch((error) => reply({ ok: false, error: error instanceof Error ? error.message : String(error) }));
