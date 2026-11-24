import { request as httpRequest } from "node:http";
import { request as httpsRequest } from "node:https";
import { readFile } from "node:fs/promises";

const deadlineMs = 1500;

function histrioEndpoint(raw) {
  const base = new URL(raw);
  if (!["http:", "https:"].includes(base.protocol) || base.username || base.password ||
      base.pathname !== "/" || base.search || base.hash) {
    throw new Error("HISTRIO_URL には HTTP(S) のオリジンを指定してください。");
  }
  return new URL("/v1/personas?namespace=di", base);
}

async function readTemplates() {
  const path = new URL("../data/debater-templates.json", import.meta.url);
  const catalog = JSON.parse(await readFile(path, "utf8"));
  if (typeof catalog.version !== "string" || !catalog.version.trim() ||
      !Array.isArray(catalog.templates) || catalog.templates.length === 0) {
    throw new Error("議論者テンプレートの版または一覧がありません。");
  }
  const seen = new Set();
  return catalog.templates.map(({ id, name, description, speech_style, traits, script_pattern, histrio_seed }) => {
    if (typeof id !== "string" || !/^tpl-[A-Za-z0-9_.-]{1,124}$/.test(id) || seen.has(id) ||
        ![name, description, speech_style, script_pattern, histrio_seed]
          .every((value) => typeof value === "string" && value.trim()) ||
        !Array.isArray(traits) || !traits.length ||
        !traits.every((value) => typeof value === "string" && value.trim())) {
      throw new Error("議論者テンプレートに不正な項目または重複 ID があります。");
    }
    seen.add(id);
    return { id, name, description, speech_style, traits, script_pattern_id: script_pattern, seed: histrio_seed };
  });
}

function upsertPersona(endpoint, persona) {
  const body = JSON.stringify(persona);
  const request = endpoint.protocol === "https:" ? httpsRequest : httpRequest;
  return new Promise((resolve, reject) => {
    const outgoing = request(endpoint, {
      method: "POST",
      agent: false,
      signal: AbortSignal.timeout(deadlineMs),
      headers: { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(body) },
    }, (response) => {
      // The status reports the committed upsert. The script needs no response data.
      const status = response.statusCode;
      response.destroy();
      if (status === 200) resolve();
      else reject(new Error(`HTTP ${status}`));
    });
    outgoing.once("error", (error) => reject(new Error(error.code ?? "接続失敗")));
    outgoing.end(body);
  });
}

async function main() {
  if (process.argv.length !== 2) throw new Error("引数を取らないコマンドです。");
  if (!process.env.HISTRIO_URL?.trim()) throw new Error("HISTRIO_URL を指定してください。");
  const endpoint = histrioEndpoint(process.env.HISTRIO_URL.trim());
  const templates = await readTemplates();
  let failed = 0;
  for (const template of templates) {
    try {
      await upsertPersona(endpoint, template);
    } catch (error) {
      failed += 1;
      console.error(`登録失敗 (${template.id}) ${error.message}`);
    }
  }
  console.log(`Histrio (di) テンプレート登録・更新 ${templates.length - failed} 件、失敗 ${failed} 件`);
  if (failed > 0) process.exitCode = 1;
}

main().catch((error) => {
  console.error(error.message);
  process.exitCode = 1;
});
