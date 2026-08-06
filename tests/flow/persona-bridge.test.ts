import assert from "node:assert/strict";
import { generateKeyPairSync, sign as signBytes, type KeyObject } from "node:crypto";
import Database from "better-sqlite3";
import { Hono } from "hono";

import {
  createPersonaBridgeRoutes,
  PERSONA_BRIDGE_ASSERTION_HEADER,
} from "../../src/api/persona-bridge-routes.js";
import { parsePersonaDocument } from "../../src/flow/persona-import-document.js";
import { buildPersonaDescriptor } from "../../src/flow/persona-descriptor.js";
import { pullVoluptasPersonas } from "../../src/flow/voluptas-persona-client.js";
import {
  PERSONA_BRIDGE_AUDIENCE,
} from "../../src/persona-bridge/authorization-assertion.js";
import {
  consumePersonaBridgeAssertion,
} from "../../src/persona-bridge/assertion-replay-store.js";
import {
  decodeUtteranceCursor,
  nextUtteranceCursor,
  readHumanUtterances,
} from "../../src/persona-bridge/utterance-export.js";

const NOW_MS = Date.parse("2026-07-28T00:00:00.000Z");

assert.equal(PERSONA_BRIDGE_ASSERTION_HEADER, "x-discutere-persona-assertion");

function signedAssertion(
  privateKey: KeyObject,
  {
    authorId = "123456789",
    audience = PERSONA_BRIDGE_AUDIENCE,
    expiresAt = Math.floor(NOW_MS / 1_000) + 120,
    jti,
  }: {
    authorId?: string;
    audience?: string;
    expiresAt?: number;
    jti: string;
  }
): string {
  const payload = Buffer.from(JSON.stringify({
    authorId,
    aud: audience,
    exp: expiresAt,
    jti,
  }), "utf8").toString("base64url");
  const signature = signBytes(null, Buffer.from(payload, "utf8"), privateKey);
  return `${payload}.${signature.toString("base64url")}`;
}

const descriptor = buildPersonaDescriptor({
  id: "p1",
  name: "Player",
  role: "opinion",
  speechStyle: "",
  traits: [],
  affectVector: new Array(20).fill(0.2),
  origin: "imported",
  parentIds: [],
  preferenceAxes: {
    "style.explorer": 0.9,
    "style.narrative": 0.8,
    "style.routine_tolerance": 0.1,
  },
  attributes: { ageBand: "20s", spending: "light" },
  aversions: [{ target: "mechanic:core/gacha", strength: 0.8 }],
  mechanicReactions: [{ mechanicId: "action/dodge-roll", sentiment: 1.5 }],
});
assert.match(descriptor, /探索と物語を強く好む/);
assert.match(descriptor, /反復は好まない/);
assert.match(descriptor, /core\/gachaを嫌う/);
assert.match(descriptor, /20s \/ light/);
console.log("  [ok] persona bridge descriptor uses deterministic v2 compartments");

const document = parsePersonaDocument('{"pseudoId":"a"}\nnot-json\n{"pseudoId":"b"}\n');
assert.equal(document.personas.length, 2);
assert.equal(document.invalidJsonLines, 1);

let requests = 0;
const pulled = await pullVoluptasPersonas({
  url: "https://voluptas.test/api/personas/export",
  token: "secret",
  fetchImpl: (async (_url: URL | RequestInfo, init?: RequestInit) => {
    requests += 1;
    assert.equal((init?.headers as Record<string, string>).authorization, "Bearer secret");
    assert.equal(init?.redirect, "manual");
    return new Response(
      requests === 1 ? '{"pseudoId":"a"}\n' : '{"pseudoId":"b"}\n',
      {
        status: 200,
        headers: requests === 1 ? { "x-next-cursor": "cursor-1" } : {},
      }
    );
  }) as typeof fetch,
});
assert.equal(pulled.pages, 2);
assert.equal(pulled.personas.length, 2);
await assert.rejects(
  pullVoluptasPersonas({
    url: "http://voluptas.test/api/personas/export",
    token: "secret",
    fetchImpl: (async () => {
      throw new Error("cleartext request must not be sent");
    }) as typeof fetch,
  }),
  /HTTPS/
);
await assert.rejects(
  pullVoluptasPersonas({
    url: "https://voluptas.test/api/personas/export",
    token: "secret",
    fetchImpl: (async () => new Response(null, {
      status: 302,
      headers: { location: "https://redirected.test/export" },
    })) as typeof fetch,
  }),
  /status 302/
);
console.log("  [ok] persona bridge pulls paged NDJSON without logging credentials");

const db = new Database(":memory:");
db.exec(`CREATE TABLE sessions (
  id TEXT PRIMARY KEY,
  workspace_id TEXT,
  title TEXT,
  scene TEXT
)`);
db.exec(`CREATE TABLE utterances (
  id TEXT PRIMARY KEY,
  workspace_id TEXT,
  session_id TEXT,
  speaker_id TEXT,
  raw_content TEXT,
  posted_at INTEGER
)`);
const insertSession = db.prepare("INSERT INTO sessions VALUES (?, ?, ?, ?)");
insertSession.run("discord", "knowledge", "discord-session:guild:channel", "discord:guild/channel");
// Web chat can mutate its scene through the generic /scene command; its trusted ingress title must
// still prevent a numeric display-name spoof from becoming Discord evidence.
insertSession.run("web", "knowledge", "web-session:lobby", "discord:spoofed/channel");
const insert = db.prepare("INSERT INTO utterances VALUES (?, ?, ?, ?, ?, ?)");
insert.run("human", "knowledge", "discord", "123456789", "本人の発言", 1000);
insert.run("web-spoof", "knowledge", "web", "123456789", "表示名を偽装した発言", 1000);
insert.run("persona", "knowledge", "discord", "persona:one", "AIの発言", 1001);
insert.run("external", "knowledge", "discord", "ext:steam:123456789", "他所の発言", 1002);
assert.deepEqual(readHumanUtterances(db, {
  authorId: "123456789",
  after: { postedAt: 0, id: "" },
  limit: 10,
}), [{
  id: "human",
  text: "本人の発言",
  createdAt: "1970-01-01T00:00:01.000Z",
}]);

for (const id of ["page-1", "page-2", "page-3"]) {
  insert.run(id, "knowledge", "discord", "555555555", id, 2000);
}
const firstPage = readHumanUtterances(db, {
  authorId: "555555555",
  after: { postedAt: 0, id: "" },
  limit: 2,
});
const nextCursor = nextUtteranceCursor(firstPage);
assert.ok(nextCursor);
const secondPage = readHumanUtterances(db, {
  authorId: "555555555",
  after: decodeUtteranceCursor(nextCursor),
  limit: 2,
});
assert.deepEqual(
  [...firstPage, ...secondPage].map((utterance) => utterance.id),
  ["page-1", "page-2", "page-3"],
  "equal-timestamp pages advance by utterance id without repeats"
);
db.close();

const calls: unknown[] = [];
const bridgeToken = "bridge-secret-0123456789abcdef0123456789";
const { publicKey, privateKey } = generateKeyPairSync("ed25519");
const assertionPublicKey = Buffer.from(
  publicKey.export({ format: "der", type: "spki" })
).toString("base64url");
const consumed = new Set<string>();
const app = new Hono();
app.route("/api", createPersonaBridgeRoutes({
  token: () => bridgeToken,
  assertionPublicKey: () => assertionPublicKey,
  now: () => NOW_MS,
  consumeAssertion: (assertion) => {
    if (consumed.has(assertion.jti)) return false;
    consumed.add(assertion.jti);
    return true;
  },
  readUtterances: (input) => {
    calls.push(input);
    return [
      { id: "u1", text: "本人", createdAt: "2026-07-28T00:00:00.000Z" },
      { id: "u2", text: "次頁", createdAt: "2026-07-28T00:00:01.000Z" },
    ].slice(0, input.limit);
  },
}));
assert.equal((await app.request(
  "/api/persona-bridge/utterances?authorId=123456789"
)).status, 401);
assert.equal((await app.request(
  "/api/persona-bridge/utterances?authorId=123456789",
  { headers: { authorization: `Bearer ${bridgeToken}` } }
)).status, 401, "Bearer credential alone must not authorize an authorId");

// 短すぎる token は「設定済み」と見なさない (推測可能な鍵で本人の発話を出さない)。
const weak = new Hono();
weak.route("/api", createPersonaBridgeRoutes({
  token: () => "short",
  readUtterances: () => {
    throw new Error("must not read utterances for a weak token");
  },
}));
assert.equal((await weak.request(
  "/api/persona-bridge/utterances?authorId=123456789",
  { headers: { authorization: "Bearer short" } }
)).status, 503);

const missingAssertionKey = new Hono();
missingAssertionKey.route("/api", createPersonaBridgeRoutes({
  token: () => bridgeToken,
  assertionPublicKey: () => "",
  readUtterances: () => {
    throw new Error("must not read utterances without an assertion public key");
  },
}));
assert.equal((await missingAssertionKey.request(
  "/api/persona-bridge/utterances?authorId=123456789",
  { headers: { authorization: `Bearer ${bridgeToken}` } }
)).status, 503);

const response = await app.request(
  "/api/persona-bridge/utterances?authorId=123456789&since=2026-07-27T00:00:00.000Z&limit=1",
  { headers: {
    authorization: `Bearer ${bridgeToken}`,
    [PERSONA_BRIDGE_ASSERTION_HEADER]: signedAssertion(privateKey, {
      jti: "assertion-valid-0001",
    }),
  } }
);
assert.equal(response.status, 200);
assert.equal(response.headers.get("cache-control"), "private, no-store");
const payload = await response.json() as {
  utterances: Array<{ text: string; createdAt: string; id?: unknown }>;
  nextCursor: string;
};
assert.equal(payload.utterances.length, 1);
assert.equal(payload.utterances[0]?.id, undefined, "internal utterance ids are not exported");
assert.deepEqual(decodeUtteranceCursor(payload.nextCursor), {
  postedAt: NOW_MS,
  id: "u1",
});
assert.equal(calls.length, 1);
assert.equal((calls[0] as { limit: number }).limit, 2, "reader gets one bounded lookahead row");

const exactPage = new Hono();
exactPage.route("/api", createPersonaBridgeRoutes({
  token: () => bridgeToken,
  assertionPublicKey: () => assertionPublicKey,
  now: () => NOW_MS,
  consumeAssertion: () => true,
  readUtterances: () => [
    { id: "only", text: "最終頁", createdAt: "2026-07-28T00:00:00.000Z" },
  ],
}));
const exactPageResponse = await exactPage.request(
  "/api/persona-bridge/utterances?authorId=123456789&limit=1",
  { headers: {
    authorization: `Bearer ${bridgeToken}`,
    [PERSONA_BRIDGE_ASSERTION_HEADER]: signedAssertion(privateKey, {
      jti: "assertion-exact-page1",
    }),
  } }
);
assert.equal(exactPageResponse.status, 200);
assert.equal((await exactPageResponse.json() as { nextCursor: string | null }).nextCursor, null);

assert.equal((await app.request(
  "/api/persona-bridge/utterances?authorId=123456789&limit=1001",
  { headers: { authorization: `Bearer ${bridgeToken}` } }
)).status, 400, "out-of-contract limits are rejected instead of clamped");

const replayAssertion = signedAssertion(privateKey, { jti: "assertion-replay-001" });
const replayHeaders = {
  authorization: `Bearer ${bridgeToken}`,
  [PERSONA_BRIDGE_ASSERTION_HEADER]: replayAssertion,
};
assert.equal((await app.request(
  "/api/persona-bridge/utterances?authorId=123456789",
  { headers: replayHeaders }
)).status, 200);
assert.equal((await app.request(
  "/api/persona-bridge/utterances?authorId=123456789",
  { headers: replayHeaders }
)).status, 401, "an assertion is one-time even while it remains unexpired");

for (const assertion of [
  signedAssertion(privateKey, { authorId: "987654321", jti: "assertion-author-01" }),
  signedAssertion(privateKey, { audience: "another-service", jti: "assertion-audience1" }),
  signedAssertion(privateKey, {
    expiresAt: Math.floor(NOW_MS / 1_000),
    jti: "assertion-expired-1",
  }),
]) {
  assert.equal((await app.request(
    "/api/persona-bridge/utterances?authorId=123456789",
    { headers: {
      authorization: `Bearer ${bridgeToken}`,
      [PERSONA_BRIDGE_ASSERTION_HEADER]: assertion,
    } }
  )).status, 401);
}
assert.equal((await app.request(
  "/api/persona-bridge/utterances?authorId=123456789",
  { headers: {
    authorization: `Bearer ${bridgeToken}`,
    [PERSONA_BRIDGE_ASSERTION_HEADER]: `${"A".repeat(3_000)}.${"A".repeat(86)}`,
  } }
)).status, 401, "oversized assertions are rejected before signature work");

const nonceDb = new Database(":memory:");
nonceDb.exec(`CREATE TABLE persona_bridge_assertion_nonce (
  jti TEXT PRIMARY KEY,
  expires_at INTEGER NOT NULL
)`);
const persistedAssertion = {
  authorId: "123456789",
  expiresAt: Math.floor(NOW_MS / 1_000) + 120,
  jti: "assertion-persist-001",
};
assert.equal(consumePersonaBridgeAssertion(nonceDb, persistedAssertion, NOW_MS), true);
assert.equal(consumePersonaBridgeAssertion(nonceDb, persistedAssertion, NOW_MS), false);
nonceDb.close();

// クエリ不正は 400、読み出し失敗は 500 + 定型文 (DB パス等の内部情報を外へ出さない)。
assert.equal((await app.request(
  "/api/persona-bridge/utterances?authorId=123456789&since=not-a-date",
  { headers: {
    authorization: `Bearer ${bridgeToken}`,
    [PERSONA_BRIDGE_ASSERTION_HEADER]: signedAssertion(privateKey, {
      jti: "assertion-invalid-query",
    }),
  } }
)).status, 400);

const broken = new Hono();
broken.route("/api", createPersonaBridgeRoutes({
  token: () => bridgeToken,
  assertionPublicKey: () => assertionPublicKey,
  now: () => NOW_MS,
  consumeAssertion: () => true,
  readUtterances: () => {
    throw new Error("SQLITE_CANTOPEN: unable to open database file /srv/discutere/kg.db");
  },
}));
const failure = await broken.request(
  "/api/persona-bridge/utterances?authorId=123456789",
  { headers: {
    authorization: `Bearer ${bridgeToken}`,
    [PERSONA_BRIDGE_ASSERTION_HEADER]: signedAssertion(privateKey, {
      jti: "assertion-reader-fail1",
    }),
  } }
);
assert.equal(failure.status, 500);
assert.equal((await failure.json() as { error: string }).error, "failed to read utterances");
console.log("  [ok] persona bridge utterance API requires token + one-time author assertion");

const { assertCrawlerVectorSpec } = await import(
  "../../src/crawler/sentiment/vector-spec.mjs"
) as {
  assertCrawlerVectorSpec: (dimensions: string[], version: number) => void;
};
const { VECTOR_DIMS, VECTOR_SPEC_VERSION } = await import("@ludiars/sentiment-core");
assert.doesNotThrow(() => assertCrawlerVectorSpec(VECTOR_DIMS, VECTOR_SPEC_VERSION));
assert.throws(() => assertCrawlerVectorSpec([...VECTOR_DIMS].reverse(), VECTOR_SPEC_VERSION), /drift/);
console.log("  [ok] crawler vector specification asserts the shared canonical order");
