/**
 * 送り側: Cernere `POST /api/auth/service-token` で service token を取得する。
 *
 * - project client credentials は Cernere にだけ送る (baseUrl は HTTPS、 開発用 loopback のみ HTTP)。
 * - token は `exp - 60 秒` まで process memory にだけキャッシュし、 ディスク・ログへ書かない。
 * - 失敗は理由コードで返し、 呼び出し側が固定トークンへのフォールバック (P4 移行期間のみ) を判断する。
 */
const REFRESH_MARGIN_MS = 60 * 1000;
const LOOPBACK_HOSTS = new Set(["localhost", "127.0.0.1", "[::1]"]);

export type ServiceTokenFailureReason =
  | "base_url_missing"
  | "base_url_insecure"
  | "credentials_missing"
  | "unauthorized"
  | "scope_undeclared"
  | "target_not_found"
  | "issuer_error"
  | "malformed_response"
  | "network";

export type ServiceTokenResult =
  | { ok: true; token: string }
  | { ok: false; reason: ServiceTokenFailureReason };

export type ServiceTokenProvider = () => Promise<ServiceTokenResult>;

export interface ServiceTokenClientConfig {
  baseUrl: string;
  clientId: string;
  clientSecret: string;
}

function reasonForStatus(status: number): ServiceTokenFailureReason {
  if (status === 401) return "unauthorized";
  if (status === 403) return "scope_undeclared";
  if (status === 404) return "target_not_found";
  return "issuer_error";
}

function issuerUrl(baseUrl: string): URL | ServiceTokenFailureReason {
  if (!baseUrl.trim()) return "base_url_missing";
  let url: URL;
  try {
    url = new URL("/api/auth/service-token", baseUrl);
  } catch {
    return "base_url_missing";
  }
  const secure = url.protocol === "https:"
    || (url.protocol === "http:" && LOOPBACK_HOSTS.has(url.hostname));
  return secure ? url : "base_url_insecure";
}

export function createServiceTokenProvider({
  targetProjectKey,
  config,
  fetchImpl = fetch,
  now = () => Date.now(),
}: {
  targetProjectKey: string;
  config: () => ServiceTokenClientConfig;
  fetchImpl?: typeof fetch;
  now?: () => number;
}): ServiceTokenProvider {
  let cached: { token: string; refreshAt: number } | null = null;
  return async () => {
    if (cached && now() < cached.refreshAt) return { ok: true, token: cached.token };
    cached = null;
    const { baseUrl, clientId, clientSecret } = config();
    if (!clientId || !clientSecret) return { ok: false, reason: "credentials_missing" };
    const url = issuerUrl(baseUrl);
    if (typeof url === "string") return { ok: false, reason: url };
    let response: Response;
    try {
      response = await fetchImpl(url, {
        method: "POST",
        redirect: "manual",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({
          client_id: clientId,
          client_secret: clientSecret,
          target_project_key: targetProjectKey,
        }),
      });
    } catch {
      return { ok: false, reason: "network" };
    }
    if (!response.ok) return { ok: false, reason: reasonForStatus(response.status) };
    let body: { accessToken?: unknown; expiresIn?: unknown };
    try {
      body = await response.json() as typeof body;
    } catch {
      return { ok: false, reason: "malformed_response" };
    }
    const { accessToken, expiresIn } = body;
    if (typeof accessToken !== "string" || !accessToken
      || typeof expiresIn !== "number" || !Number.isFinite(expiresIn) || expiresIn <= 0) {
      return { ok: false, reason: "malformed_response" };
    }
    cached = { token: accessToken, refreshAt: now() + expiresIn * 1000 - REFRESH_MARGIN_MS };
    return { ok: true, token: accessToken };
  };
}

/** Excubitor `cernere_launch_credentials` が注入する env から読む。 */
export function serviceTokenClientConfigFromEnv(
  env: NodeJS.ProcessEnv = process.env
): ServiceTokenClientConfig {
  return {
    baseUrl: env.CERNERE_BASE_URL ?? "",
    clientId: env.CERNERE_PROJECT_CLIENT_ID ?? "",
    clientSecret: env.CERNERE_PROJECT_CLIENT_SECRET ?? "",
  };
}
