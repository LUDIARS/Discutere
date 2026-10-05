import assert from "node:assert/strict";
import {
  generateKeyPairSync,
  sign as signBytes,
  type KeyObject,
} from "node:crypto";
import { Hono } from "hono";

import {
  createPersonaBridgeRoutes,
  PERSONA_BRIDGE_ASSERTION_HEADER,
  PERSONA_BRIDGE_SERVICE_SCOPE,
} from "../../src/api/persona-bridge-routes.js";
import {
  createCernerePublicKeyLoader,
  parseWellKnownKeys,
} from "../../src/cernere-service-token/cernere-public-keys.js";
import { pae, verifyPasetoV4Public } from "../../src/cernere-service-token/paseto-v4-public.js";
import { createServiceTokenProvider } from "../../src/cernere-service-token/service-token-client.js";
import { verifyServiceToken } from "../../src/cernere-service-token/service-token-verify.js";
import {
  pullVoluptasPersonas,
  resolveVoluptasBearer,
  VOLUPTAS_TARGET_PROJECT_KEY,
} from "../../src/flow/voluptas-persona-client.js";
import { PERSONA_BRIDGE_AUDIENCE } from "../../src/persona-bridge/authorization-assertion.js";

const NOW_MS = Date.parse("2026-07-28T00:05:00.000Z");
const AUDIENCE = "discutere";

// ── PASETO v4.public: Cernere と同じ paseto ライブラリ (V4.sign) で作った token を検証できる ──
// seed=0x07*32, kid=v1。 生成は Cernere/server の paseto@3 で行い、 結果だけを固定値にした。
const FIXTURE_PUBLIC_KEY = "6kpsY+KcUgq+9VB7Ey7F+ZVHdq6+vnuSQh7qaRRG0iw=";
const FIXTURE_TOKEN = "v4.public.eyJraW5kIjoic2VydmljZSIsInN1YiI6InZvbHB1dGFzIiwiYXVkIjoiZGlzY3V0ZXJlIiwic2NvcGUiOlsicGVyc29uYS1icmlkZ2U6d3JpdGUiXSwiaWF0IjoiMjAyNi0wNy0yOFQwMDowMDowMC4wMDBaIiwiZXhwIjoiMjAyNi0wNy0yOFQwMDoxNTowMC4wMDBaIiwianRpIjoiZml4dHVyZS0wMDAxIiwia2lkIjoidjEifW4L7tWdrGRscubWbovf5pAXsHHgM85L4pYlgsyEXrNx4Fusewx0hq-dGVdPinvbkRbxjhl9uYtT9ryA6apZCwM";
const fixtureKeys = parseWellKnownKeys({
  keys: [{ kid: "v1", alg: "EdDSA", public_key: FIXTURE_PUBLIC_KEY, current: true }],
});
assert.equal(verifyPasetoV4Public(FIXTURE_TOKEN, fixtureKeys).aud, "discutere");
assert.throws(() => verifyPasetoV4Public(`${FIXTURE_TOKEN.slice(0, -2)}AA`, fixtureKeys));
console.log("  [ok] PASETO v4.public verifier accepts tokens signed by the paseto library");

// ── テスト用の Cernere 署名鍵 ──
const cernere = generateKeyPairSync("ed25519");
const otherIssuer = generateKeyPairSync("ed25519");
function rawPublicKey(key: KeyObject): string {
  return Buffer.from(key.export({ format: "der", type: "spki" })).subarray(12).toString("base64");
}
const cernereKeys = parseWellKnownKeys({ keys: [{ public_key: rawPublicKey(cernere.publicKey) }] });
const loadKeys = async () => cernereKeys;

function serviceToken(
  claims: Record<string, unknown>,
  privateKey: KeyObject = cernere.privateKey
): string {
  const message = Buffer.from(JSON.stringify({
    kind: "service",
    sub: "volputas",
    aud: AUDIENCE,
    scope: [PERSONA_BRIDGE_SERVICE_SCOPE],
    iat: "2026-07-28T00:00:00.000Z",
    exp: "2026-07-28T00:15:00.000Z",
    jti: "svc-0001",
    ...claims,
  }), "utf8");
  const signature = signBytes(
    null,
    pae([Buffer.from("v4.public.", "utf8"), message, Buffer.alloc(0), Buffer.alloc(0)]),
    privateKey
  );
  return `v4.public.${Buffer.concat([message, signature]).toString("base64url")}`;
}

async function verdictOf(token: string, requiredScope = PERSONA_BRIDGE_SERVICE_SCOPE) {
  return verifyServiceToken({ token, audience: AUDIENCE, requiredScope, loadKeys, nowMs: NOW_MS });
}

assert.equal((await verdictOf(serviceToken({}))).ok, true);
assert.deepEqual(await verdictOf(serviceToken({ aud: "glab" })), {
  ok: false, status: 401, reason: "audience_mismatch",
});
assert.deepEqual(await verdictOf(serviceToken({ exp: "2026-07-28T00:04:59.000Z" })), {
  ok: false, status: 401, reason: "expired",
});
assert.deepEqual(await verdictOf(serviceToken({ kind: "user_for_project" })), {
  ok: false, status: 401, reason: "not_service_token",
});
assert.deepEqual(await verdictOf(serviceToken({}, otherIssuer.privateKey)), {
  ok: false, status: 401, reason: "invalid_signature",
});
assert.deepEqual(await verdictOf(serviceToken({ scope: ["persona-export:read"] })), {
  ok: false, status: 403, reason: "scope_missing",
});
// 呼出元名 (sub) では分岐しない: scope と aud が揃っていれば sub が何でも通る。
assert.equal((await verdictOf(serviceToken({ sub: "some-other-service" }))).ok, true);
assert.deepEqual(
  await verifyServiceToken({
    token: serviceToken({}), audience: "", requiredScope: PERSONA_BRIDGE_SERVICE_SCOPE, loadKeys, nowMs: NOW_MS,
  }),
  { ok: false, status: 503, reason: "audience_not_configured" }
);
console.log("  [ok] service token verification checks signature, kind, exp, aud and scope");

// ── 公開鍵 loader: 10 分キャッシュし、 Cernere の well-known からだけ取る ──
let keyFetches = 0;
let clock = NOW_MS;
const keyLoader = createCernerePublicKeyLoader({
  baseUrl: () => "http://127.0.0.1:9999",
  now: () => clock,
  fetchImpl: (async (url: URL | RequestInfo) => {
    keyFetches += 1;
    assert.equal(String(url), "http://127.0.0.1:9999/.well-known/cernere-public-key");
    return Response.json({ keys: [{ public_key: rawPublicKey(cernere.publicKey) }] });
  }) as typeof fetch,
});
await keyLoader();
await keyLoader();
assert.equal(keyFetches, 1);
clock += 10 * 60 * 1000;
await keyLoader();
assert.equal(keyFetches, 2);
await assert.rejects(createCernerePublicKeyLoader({ baseUrl: () => "" })(), /CERNERE_BASE_URL/);
console.log("  [ok] cernere public keys are cached in memory");

// ── persona bridge route: 新旧両受理 ──
const bridgeToken = "bridge-secret-0123456789abcdef0123456789";
const assertionKeys = generateKeyPairSync("ed25519");
const assertionPublicKey = Buffer.from(
  assertionKeys.publicKey.export({ format: "der", type: "spki" })
).toString("base64url");
let assertionSeq = 0;
function assertion(): string {
  assertionSeq += 1;
  const payload = Buffer.from(JSON.stringify({
    authorId: "123456789",
    aud: PERSONA_BRIDGE_AUDIENCE,
    exp: Math.floor(NOW_MS / 1_000) + 120,
    jti: `svc-assertion-${String(assertionSeq).padStart(4, "0")}`,
  }), "utf8").toString("base64url");
  const signature = signBytes(null, Buffer.from(payload, "utf8"), assertionKeys.privateKey);
  return `${payload}.${signature.toString("base64url")}`;
}

function bridgeApp(options: { token?: string; audience?: string } = {}): Hono {
  const app = new Hono();
  app.route("/api", createPersonaBridgeRoutes({
    token: () => options.token ?? bridgeToken,
    serviceAudience: () => options.audience ?? AUDIENCE,
    loadServiceTokenKeys: loadKeys,
    assertionPublicKey: () => assertionPublicKey,
    consumeAssertion: () => true,
    now: () => NOW_MS,
    readUtterances: () => [{ id: "u1", text: "本人", createdAt: "2026-07-28T00:00:00.000Z" }],
  }));
  return app;
}

async function statusFor(app: Hono, bearer: string, withAssertion = true): Promise<number> {
  const headers: Record<string, string> = { authorization: `Bearer ${bearer}` };
  if (withAssertion) headers[PERSONA_BRIDGE_ASSERTION_HEADER] = assertion();
  return (await app.request("/api/persona-bridge/utterances?authorId=123456789", { headers })).status;
}

const app = bridgeApp();
assert.equal(await statusFor(app, serviceToken({})), 200, "service token is accepted");
assert.equal(await statusFor(app, bridgeToken), 200, "fixed token is still accepted during P4");
assert.equal(await statusFor(app, serviceToken({ scope: [] })), 403);
assert.equal(await statusFor(app, serviceToken({ aud: "volputas" })), 401);
assert.equal(await statusFor(app, serviceToken({}, otherIssuer.privateKey)), 401);
assert.equal(await statusFor(app, "wrong-fixed-token-0123456789abcdef0123"), 401);
// assertion (本人同意の在席証明) は service token でも従来どおり必須。
assert.equal(await statusFor(app, serviceToken({}), false), 401);
// 固定トークン未設定でも service token は通る。 固定トークン経路は従来どおり 503。
const serviceOnly = bridgeApp({ token: "" });
assert.equal(await statusFor(serviceOnly, serviceToken({})), 200);
assert.equal(await statusFor(serviceOnly, "any-fixed-token-0123456789abcdef01234"), 503);
// 自分の storage_slug 未設定なら service token は判定できないので 503 (fail-closed)。
assert.equal(await statusFor(bridgeApp({ audience: "" }), serviceToken({})), 503);
console.log("  [ok] persona bridge accepts Cernere service token and the fixed token");

// ── 送り側: service token 発行 client ──
let issued = 0;
let issueClock = NOW_MS;
const issueRequests: Array<{ url: string; body: Record<string, string> }> = [];
const provider = createServiceTokenProvider({
  targetProjectKey: VOLUPTAS_TARGET_PROJECT_KEY,
  config: () => ({ baseUrl: "https://cernere.test", clientId: "client-1", clientSecret: "secret-1" }),
  now: () => issueClock,
  fetchImpl: (async (url: URL | RequestInfo, init?: RequestInit) => {
    issued += 1;
    issueRequests.push({ url: String(url), body: JSON.parse(String(init?.body)) });
    return Response.json({ tokenType: "service", accessToken: `svc-token-${issued}`, expiresIn: 900 });
  }) as typeof fetch,
});
assert.deepEqual(await provider(), { ok: true, token: "svc-token-1" });
issueClock += (900 - 61) * 1000;
assert.deepEqual(await provider(), { ok: true, token: "svc-token-1" }, "cached until exp - 60s");
issueClock += 2 * 1000;
assert.deepEqual(await provider(), { ok: true, token: "svc-token-2" }, "refreshed before exp");
assert.deepEqual(issueRequests[0], {
  url: "https://cernere.test/api/auth/service-token",
  body: { client_id: "client-1", client_secret: "secret-1", target_project_key: "volputas" },
});

async function failureFor(
  config: { baseUrl: string; clientId: string; clientSecret: string },
  respond: () => Promise<Response>
) {
  return createServiceTokenProvider({
    targetProjectKey: "volputas",
    config: () => config,
    fetchImpl: respond as unknown as typeof fetch,
  })();
}
const creds = { baseUrl: "https://cernere.test", clientId: "c", clientSecret: "s" };
const neverCalled = async (): Promise<Response> => {
  throw new Error("credentials must not be sent");
};
assert.deepEqual(await failureFor({ ...creds, clientSecret: "" }, neverCalled), {
  ok: false, reason: "credentials_missing",
});
assert.deepEqual(await failureFor({ ...creds, baseUrl: "http://cernere.test" }, neverCalled), {
  ok: false, reason: "base_url_insecure",
});
assert.deepEqual(await failureFor(creds, async () => new Response(null, { status: 401 })), {
  ok: false, reason: "unauthorized",
});
assert.deepEqual(await failureFor(creds, async () => new Response(null, { status: 403 })), {
  ok: false, reason: "scope_undeclared",
});
assert.deepEqual(await failureFor(creds, async () => new Response(null, { status: 404 })), {
  ok: false, reason: "target_not_found",
});
assert.deepEqual(await failureFor(creds, async () => {
  throw new TypeError("fetch failed");
}), { ok: false, reason: "network" });
console.log("  [ok] service token client caches until exp - 60s and reports failure reasons");

// ── 送り側: 発行失敗時だけ固定トークンへフォールバックし、 秘密値をログに出さない ──
const logs: string[] = [];
assert.equal(await resolveVoluptasBearer({
  serviceToken: async () => ({ ok: true, token: "svc" }),
  fallbackToken: "fixed",
  log: (line) => logs.push(line),
}), "svc");
assert.equal(logs.length, 0);
assert.equal(await resolveVoluptasBearer({
  serviceToken: async () => ({ ok: false, reason: "scope_undeclared" }),
  fallbackToken: "fixed-secret-value",
  log: (line) => logs.push(line),
}), "fixed-secret-value");
assert.equal(logs.length, 1);
assert.match(logs[0]!, /reason=scope_undeclared/);
assert.doesNotMatch(logs[0]!, /fixed-secret-value/);
await assert.rejects(resolveVoluptasBearer({
  serviceToken: async () => ({ ok: false, reason: "network" }),
  fallbackToken: "",
}), /reason=network/);

const sentAuthorization: string[] = [];
await pullVoluptasPersonas({
  url: "https://voluptas.test/api/personas/export",
  token: "fixed",
  serviceToken: async () => ({ ok: true, token: "svc-token" }),
  fetchImpl: (async (_url: URL | RequestInfo, init?: RequestInit) => {
    sentAuthorization.push((init?.headers as Record<string, string>).authorization);
    return new Response('{"pseudoId":"a"}\n', { status: 200 });
  }) as typeof fetch,
});
assert.deepEqual(sentAuthorization, ["Bearer svc-token"]);
console.log("  [ok] Voluptas pull prefers the service token and falls back only on issuance failure");
