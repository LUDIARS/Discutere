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
import { getGitHubCliAuthStatus, getGitHubCliToken } from "../github-cli.js";
import {
  buildGithubSpecSource,
  extractSpecTextFromBytes,
  fetchGithubRepoInfo,
  fetchGithubSpecText,
  isSupportedSpecFileName,
  DEFAULT_MAX_SPEC_BYTES,
} from "../spec-source.js";

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
  if (!isSupportedSpecFileName(fileName)) {
    return c.json({ ok: false, error: `unsupported file format: ${fileName}` }, 415);
  }

  let text: string;
  try {
    text = await extractSpecTextFromBytes(bytes, fileName);
  } catch (e) {
    return c.json({ ok: false, error: (e as Error).message }, 422);
  }

  return c.json({ ok: true, text });
});

specExtractRoutes.get("/api/spec/github/auth", async (c) => {
  const auth = await getGitHubCliAuthStatus();
  return c.json({ ok: true, auth });
});

specExtractRoutes.post("/api/spec/github/check", async (c) => {
  const body = (await c.req.json().catch(() => ({}))) as {
    repoUrl?: unknown;
    path?: unknown;
    ref?: unknown;
  };
  const repoUrl = typeof body.repoUrl === "string" ? body.repoUrl.trim() : "";
  const path = typeof body.path === "string" ? body.path.trim() : "";
  const ref = typeof body.ref === "string" ? body.ref.trim() : "";
  if (!repoUrl) return c.json({ ok: false, error: "repoUrl is required" }, 400);
  const source = buildGithubSpecSource(repoUrl, path, ref);
  if (!source) return c.json({ ok: false, error: "GitHub repository URL could not be parsed" }, 400);

  const auth = await getGitHubCliAuthStatus();
  const token = await getGitHubCliToken();
  try {
    if (!source.path) {
      const repo = await fetchGithubRepoInfo(source, { githubToken: token });
      return c.json({ ok: true, readable: true, auth, repo, source });
    }
    const result = await fetchGithubSpecText(source, { githubToken: token });
    return c.json({
      ok: true,
      readable: true,
      auth,
      source: result.source,
      size: result.size,
      sha: result.sha,
      htmlUrl: result.htmlUrl,
      text: result.text,
      preview: result.text.slice(0, 1000),
    });
  } catch (e) {
    return c.json({ ok: false, readable: false, auth, source, error: (e as Error).message }, 422);
  }
});
