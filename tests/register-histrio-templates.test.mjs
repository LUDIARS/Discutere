import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { readFile } from "node:fs/promises";
import { createServer } from "node:http";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

const run = promisify(execFile);
const script = fileURLToPath(new URL("../scripts/register-histrio-templates.mjs", import.meta.url));

test("registration upserts only the versioned templates in the Di namespace", async () => {
  const { version, templates } = JSON.parse(await readFile(new URL("../data/debater-templates.json", import.meta.url), "utf8"));
  assert.ok(version);
  assert.equal(templates.length, 12);
  const countsBy = (key) => {
    const counts = new Map();
    for (const template of templates) counts.set(template[key], (counts.get(template[key]) ?? 0) + 1);
    return counts;
  };
  assert.deepEqual(countsBy("script_pattern"), new Map([
    ["after", 2], ["almost", 2], ["always", 2], ["never", 2], ["until", 2], ["open_ended", 2],
  ]));
  assert.deepEqual(countsBy("driver"), new Map([
    ["perfect", 2], ["please", 3], ["try-hard", 3], ["hurry", 2], ["be-strong", 2],
  ]));

  const requests = [];
  const stored = new Map();
  const server = createServer(async (request, response) => {
    try {
      const chunks = [];
      for await (const chunk of request) chunks.push(chunk);
      const body = JSON.parse(Buffer.concat(chunks).toString("utf8"));
      requests.push({ method: request.method, url: request.url, body });
      stored.set(body.id, body);
      response.writeHead(200).end();
    } catch {
      response.writeHead(400).end();
    }
  });
  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  try {
    const env = {
      HISTRIO_URL: "http://127.0.0.1:" + server.address().port,
      DATABASE_PATH: "missing-pool-database/should-never-be-opened.db",
    };
    for (let i = 0; i < 2; i++) await run(process.execPath, [script], { env, timeout: 25_000 });
  } finally {
    await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
  }

  const expected = templates.map(({ id, name, description, speech_style, traits, script_pattern, histrio_seed }) => ({
    method: "POST", url: "/v1/personas?namespace=di",
    body: { id, name, description, speech_style, traits, script_pattern_id: script_pattern, seed: histrio_seed },
  }));
  assert.deepEqual(requests, [...expected, ...expected]);
  assert.equal(stored.size, templates.length);
  assert.ok([...stored.keys()].every((id) => id.startsWith("tpl-")));
});
