import fs from "node:fs";
import path from "node:path";

interface InfisicalBootstrap {
  siteUrl: string;
  projectId: string;
  environment: string;
  clientId: string;
  clientSecret: string;
}

interface AuthResponse {
  accessToken: string;
}

interface RawSecret {
  secretKey: string;
  secretValue: string;
}

interface SecretsResponse {
  secrets: RawSecret[];
}

interface SecretStatus {
  bootstrapConfigured: boolean;
  cached: boolean;
  present: boolean;
}

const secretCache = new Map<string, string | null>();

function parseDotenv(content: string): Record<string, string> {
  const out: Record<string, string> = {};
  for (const rawLine of content.split(/\r?\n/u)) {
    const line = rawLine.trim();
    if (!line || line.startsWith("#")) continue;
    const eq = line.indexOf("=");
    if (eq < 0) continue;
    const key = line.slice(0, eq).trim();
    let value = line.slice(eq + 1).trim();
    if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith("'") && value.endsWith("'"))) {
      value = value.slice(1, -1);
    }
    out[key] = value;
  }
  return out;
}

export function loadInfisicalBootstrap(secretsPath = path.resolve(".env.secrets")): InfisicalBootstrap | null {
  if (!fs.existsSync(secretsPath)) return null;
  const vars = parseDotenv(fs.readFileSync(secretsPath, "utf8"));
  const siteUrl = vars.INFISICAL_SITE_URL || "https://app.infisical.com";
  const environment = vars.INFISICAL_ENVIRONMENT || "dev";
  const projectId = vars.INFISICAL_PROJECT_ID;
  const clientId = vars.INFISICAL_CLIENT_ID;
  const clientSecret = vars.INFISICAL_CLIENT_SECRET;
  if (!projectId || !clientId || !clientSecret) return null;
  return { siteUrl, projectId, environment, clientId, clientSecret };
}

async function authenticate(config: InfisicalBootstrap): Promise<string> {
  const res = await fetch(`${config.siteUrl}/api/v1/auth/universal-auth/login`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ clientId: config.clientId, clientSecret: config.clientSecret }),
  });
  if (!res.ok) throw new Error(`Infisical authentication failed: ${res.status} ${await res.text()}`);
  const data = (await res.json()) as AuthResponse;
  return data.accessToken;
}

async function fetchSecret(config: InfisicalBootstrap, token: string, key: string): Promise<string | null> {
  const params = new URLSearchParams({
    environment: config.environment,
    workspaceId: config.projectId,
    secretPath: "/",
  });
  const res = await fetch(`${config.siteUrl}/api/v3/secrets/raw?${params}`, {
    headers: { Authorization: `Bearer ${token}` },
  });
  if (!res.ok) throw new Error(`Infisical secret fetch failed: ${res.status} ${await res.text()}`);
  const data = (await res.json()) as SecretsResponse;
  return data.secrets.find((s) => s.secretKey === key)?.secretValue ?? null;
}

async function upsertSecret(config: InfisicalBootstrap, token: string, key: string, value: string): Promise<void> {
  const payload = {
    workspaceId: config.projectId,
    environment: config.environment,
    secretPath: "/",
    secretValue: value,
    type: "shared",
  };
  const url = `${config.siteUrl}/api/v3/secrets/raw/${encodeURIComponent(key)}`;
  const res = await fetch(url, {
    method: "PATCH",
    headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  if (res.ok) return;

  const create = await fetch(url, {
    method: "POST",
    headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  if (!create.ok) throw new Error(`Infisical secret create failed: ${create.status} ${await create.text()}`);
}

export async function getInfisicalRuntimeSecret(
  key: string,
  opts: { refresh?: boolean } = {}
): Promise<string | null> {
  if (!opts.refresh && secretCache.has(key)) return secretCache.get(key) ?? null;
  const bootstrap = loadInfisicalBootstrap();
  if (!bootstrap) return process.env[key] ?? null;
  const token = await authenticate(bootstrap);
  const value = await fetchSecret(bootstrap, token, key);
  secretCache.set(key, value);
  return value;
}

export async function setInfisicalRuntimeSecret(key: string, value: string): Promise<void> {
  const bootstrap = loadInfisicalBootstrap();
  if (!bootstrap) throw new Error("Infisical bootstrap is not configured (.env.secrets)");
  const token = await authenticate(bootstrap);
  await upsertSecret(bootstrap, token, key, value);
  secretCache.set(key, value);
}

export function setInfisicalRuntimeSecretCache(key: string, value: string | null): void {
  secretCache.set(key, value);
}

export function getInfisicalRuntimeSecretStatus(key: string): SecretStatus {
  const cached = secretCache.has(key);
  return {
    bootstrapConfigured: !!loadInfisicalBootstrap(),
    cached,
    present: !!secretCache.get(key),
  };
}
