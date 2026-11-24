import { request as httpRequest } from "node:http";
import { request as httpsRequest } from "node:https";
import { DatabaseSync } from "node:sqlite";

const deadlineMs = 1500;
const pageSize = 50;

function histrioEndpoint(raw) {
  const base = new URL(raw);
  if (!["http:", "https:"].includes(base.protocol) || base.username || base.password ||
      base.pathname !== "/" || base.search || base.hash) {
    throw new Error("HISTRIO_URL には HTTP(S) のオリジンを指定してください。");
  }
  return new URL("/v1/personas?namespace=di", base);
}

function personaPayload(row) {
  if (!/^[A-Za-z0-9_.-]{1,128}$/.test(row.id) || row.id === "." || row.id === "..") {
    throw new Error("プール人物の ID が Histrio の形式に適合しません。");
  }
  const parsed = JSON.parse(row.traits_json);
  if (!Array.isArray(parsed)) throw new Error("traits_json が配列ではありません。");
  const traits = parsed.filter((value) => typeof value === "string" && value.trim())
    .slice(0, 32).map((value) => value.trim().slice(0, 120));
  const name = row.name.trim().slice(0, 120) || row.id;
  const speechStyle = row.speech_style.trim().slice(0, 240);
  const description = [`Di の議論ペルソナ ${name}。`, ...traits, speechStyle].filter(Boolean).join(" ");
  return { id: row.id, name, description, speech_style: speechStyle, traits };
}

function upsertPersona(endpoint, persona) {
  const body = JSON.stringify(persona);
  const request = endpoint.protocol === "https:" ? httpsRequest : httpRequest;
  return new Promise((resolve, reject) => {
    const outgoing = request(endpoint, {
      method: "POST",
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
  const db = new DatabaseSync(process.env.DATABASE_PATH || "./data/discutere.db", { readOnly: true });
  try {
    const readPage = db.prepare(`SELECT id,
        substr(name, 1, 120) AS name,
        substr(speech_style, 1, 240) AS speech_style,
        CASE WHEN length(CAST(traits_json AS BLOB)) <= 65536 THEN traits_json ELSE '[]' END AS traits_json
      FROM flow_persona
      WHERE archived = 0 AND (? IS NULL OR id > ?)
      ORDER BY id LIMIT ?`);
    let lastId = null;
    let registered = 0;
    let failed = 0;
    for (;;) {
      const page = readPage.all(lastId, lastId, pageSize);
      if (page.length === 0) break;
      for (const row of page) {
        try {
          await upsertPersona(endpoint, personaPayload(row));
          registered += 1;
        } catch (error) {
          failed += 1;
          console.error(`登録失敗 (${JSON.stringify(row.id)}) ${error.message}`);
        }
      }
      lastId = page.at(-1).id;
    }
    console.log(`Histrio (di) 登録・更新 ${registered} 件、失敗 ${failed} 件`);
    if (failed > 0) process.exitCode = 1;
  } finally {
    db.close();
  }
}

main().catch((error) => {
  console.error(error.message);
  process.exitCode = 1;
});
