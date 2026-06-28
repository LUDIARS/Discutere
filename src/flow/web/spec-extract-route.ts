/**
 * 仕様書バイナリアップロード + テキスト抽出エンドポイント (Web ファイルアップロード用)。
 *
 *   POST /api/spec/extract   multipart/form-data { file: File }
 *   → { ok: true, text: string }
 *
 * loopback 信頼の Web UI がバイナリファイル (PDF/DOCX) を送信し、サーバ側でテキストを
 * 抽出して返す。抽出結果はクライアントが specText 欄に展開して POST /api/flow/start へ流す。
 * テキスト系ファイルも同じ経路を使える (UTF-8 デコード)。
 */

import { Hono } from "hono";
import { extractSpecTextFromBytes, DEFAULT_MAX_SPEC_BYTES } from "../spec-source.js";

export const specExtractRoutes = new Hono();

specExtractRoutes.post("/api/spec/extract", async (c) => {
  let formData: FormData;
  try {
    formData = await c.req.formData();
  } catch {
    return c.json({ ok: false, error: "multipart/form-data が必要です" }, 400);
  }

  const file = formData.get("file");
  if (!file || typeof file === "string") {
    return c.json({ ok: false, error: "file フィールドが必要です" }, 400);
  }

  const blob = file as File;
  if (blob.size > DEFAULT_MAX_SPEC_BYTES) {
    return c.json({
      ok: false,
      error: `ファイルが大きすぎます (${blob.size} > ${DEFAULT_MAX_SPEC_BYTES} bytes)`,
    }, 413);
  }

  const bytes = new Uint8Array(await blob.arrayBuffer());
  const fileName = blob.name || "upload";

  let text: string;
  try {
    text = await extractSpecTextFromBytes(bytes, fileName);
  } catch (e) {
    return c.json({ ok: false, error: (e as Error).message }, 422);
  }

  return c.json({ ok: true, text });
});
