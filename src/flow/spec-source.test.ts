import assert from "node:assert/strict";
import { test } from "node:test";
import { crc32, deflateRawSync } from "node:zlib";
import { extractSpecTextFromBytes, fetchSpecText, readSpecTextFromFile } from "./spec-source.js";

const MiB = 1024 * 1024;
const ENTRY_LIMIT = 64 * MiB;
const TOTAL_LIMIT = 128 * MiB;

interface ZipEntry {
  name: string;
  data: Buffer;
  compression?: 0 | 8;
  declaredSize?: number;
}

/** Generate fixtures in memory, including deliberately dishonest ZIP size fields. */
function zip(entries: ZipEntry[]): Buffer {
  const locals: Buffer[] = [];
  const directory: Buffer[] = [];
  let offset = 0;
  for (const entry of entries) {
    const name = Buffer.from(entry.name);
    const method = entry.compression ?? 8;
    const data = method === 8 ? deflateRawSync(entry.data) : entry.data;
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(method, 8);
    local.writeUInt32LE(crc32(entry.data), 14);
    local.writeUInt32LE(data.length, 18);
    local.writeUInt32LE(entry.declaredSize ?? entry.data.length, 22);
    local.writeUInt16LE(name.length, 26);
    locals.push(local, name, data);
    const central = Buffer.alloc(46);
    central.writeUInt32LE(0x02014b50);
    central.writeUInt16LE(20, 4);
    local.copy(central, 6, 4, 28);
    central.writeUInt32LE(offset, 42);
    directory.push(central, name);
    offset += local.length + name.length + data.length;
  }
  const cd = Buffer.concat(directory);
  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50);
  eocd.writeUInt16LE(entries.length, 8);
  eocd.writeUInt16LE(entries.length, 10);
  eocd.writeUInt32LE(cd.length, 12);
  eocd.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, cd, eocd]);
}

test("normal PPTX and XLSX extraction is unchanged for stored and deflated XML", async () => {
  for (const compression of [0, 8] as const) {
    const pptx = zip([
      { name: "ppt/slides/slide2.xml", data: Buffer.from("<a:t>二枚目 &amp; 続き</a:t>"), compression },
      { name: "ppt/slides/slide1.xml", data: Buffer.from("<a:t>一枚目</a:t>"), compression },
      { name: "ppt/notesSlides/notesSlide1.xml", data: Buffer.from("<a:t>注記</a:t>"), compression },
    ]);
    assert.equal(await extractSpecTextFromBytes(pptx, "spec.pptx"), "注記\n\n一枚目\n\n二枚目 & 続き");
    const xlsx = zip([
      { name: "xl/worksheets/sheet1.xml", data: Buffer.from("<c><v>42</v></c>"), compression },
      { name: "xl/sharedStrings.xml", data: Buffer.from("<t>項目 &lt;A&gt;</t>"), compression },
    ]);
    assert.equal(await extractSpecTextFromBytes(xlsx, "spec.xlsx"), "項目 <A>\n\n42");
  }
});

test("a deflated entry at 64 MiB is allowed", async () => {
  const bytes = zip([
    { name: "padding", data: Buffer.alloc(ENTRY_LIMIT, 32) },
    { name: "ppt/slides/slide1.xml", data: Buffer.from("<t>ok</t>") },
  ]);
  assert.equal(await extractSpecTextFromBytes(bytes, "boundary.pptx"), "ok");
});

test("a forged entry exceeding 64 MiB is rejected across upload, URL and file paths", async () => {
  const bytes = zip([{ name: "xl/sharedStrings.xml", data: Buffer.alloc(ENTRY_LIMIT + 1, 32), declaredSize: 1 }]);
  assert.ok(bytes.length < 5_000_000);
  await assert.rejects(() => extractSpecTextFromBytes(bytes, "bomb.xlsx"), /^Error: ファイル bomb.xlsx: .*ZIP.*limit/);
  await assert.rejects(() => fetchSpecText("https://example.invalid/bomb.pptx", {
    fetchImpl: (async () => new Response(new Uint8Array(bytes))) as typeof fetch,
  }), /^Error: URL https:\/\/example.invalid\/bomb.pptx: .*ZIP.*limit/);
  await assert.rejects(() => readSpecTextFromFile("bomb.xlsx", {
    fs: {
      existsSync: () => true,
      statSync: (() => ({ size: bytes.length })) as unknown as typeof import("node:fs").statSync,
      readFileSync: (() => bytes) as unknown as typeof import("node:fs").readFileSync,
    },
  }), /^Error: ファイル bomb.xlsx: .*ZIP.*limit/);
});

test("aggregate limit counts duplicate names and non-text entries before inflating the next entry", async () => {
  const padding = Buffer.alloc(ENTRY_LIMIT, 32);
  const bytes = zip([
    { name: "padding", data: padding },
    { name: "padding", data: padding },
    { name: "ppt/slides/slide1.xml", data: Buffer.from("<t>overflow</t>"), declaredSize: 0 },
  ]);
  await assert.rejects(() => extractSpecTextFromBytes(bytes, "total.pptx"), /^Error: ファイル total.pptx: .*ZIP.*limit/);
});

test("aggregate output at 128 MiB and a following empty entry are allowed", async () => {
  const xml = Buffer.from("<t>ok</t>");
  const bytes = zip([
    { name: "xl/sharedStrings.xml", data: xml },
    { name: "padding", data: Buffer.alloc(ENTRY_LIMIT, 32) },
    { name: "padding", data: Buffer.alloc(TOTAL_LIMIT - ENTRY_LIMIT - xml.length, 32) },
    { name: "empty", data: Buffer.alloc(0) },
  ]);
  assert.equal(await extractSpecTextFromBytes(bytes, "total.xlsx"), "ok");
});

test("stored ZIP entries also obey the entry and aggregate budgets", async () => {
  await assert.rejects(() => extractSpecTextFromBytes(zip([
    { name: "padding", data: Buffer.alloc(ENTRY_LIMIT + 1, 32), compression: 0 },
  ]), "stored.xlsx"), /^Error: ファイル stored.xlsx: .*ZIP.*limit/);
  const padding = Buffer.alloc(ENTRY_LIMIT, 32);
  await assert.rejects(() => extractSpecTextFromBytes(zip([
    { name: "padding", data: padding },
    { name: "padding", data: padding },
    { name: "extra", data: Buffer.from("x"), compression: 0 },
  ]), "stored-total.xlsx"), /^Error: ファイル stored-total.xlsx: .*ZIP.*limit/);
});

test("invalid deflate data retains the source label", async () => {
  const bytes = zip([{ name: "ppt/slides/slide1.xml", data: Buffer.from("<t>ok</t>") }]);
  bytes[30 + Buffer.byteLength("ppt/slides/slide1.xml")] = 0xff;
  await assert.rejects(() => extractSpecTextFromBytes(bytes, "invalid.pptx"), /^Error: ファイル invalid.pptx: /);
});
