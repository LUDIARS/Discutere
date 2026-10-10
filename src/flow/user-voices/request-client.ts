/** Voluptas の永続受付 API。読み取り用固定トークンを依頼権限へ転用しない。 */
import { getConfig } from "../../config.js";
import { createServiceTokenProvider, serviceTokenClientConfigFromEnv, type ServiceTokenProvider } from "../../cernere-service-token/service-token-client.js";
import type { VoiceRequestPort } from "./preparation.js";

export class VoiceRequestFailure extends Error {
  constructor(public readonly kind: "down" | "auth" | "configuration" | "protocol", public readonly deliveryUnknown = false) {
    super(`Voluptas request: ${kind}`);
  }
}

export function createVoiceRequestClient(baseUrl: () => string, token: ServiceTokenProvider, fetchImpl: typeof fetch = fetch): VoiceRequestPort {
  async function request(requestId: string, theme?: string): Promise<{ accepted: boolean; requestId: string }> {
    let base: URL;
    try { base = new URL(baseUrl()); } catch { throw new VoiceRequestFailure("configuration"); }
    if (base.username || base.password || !(base.protocol === "https:" || (base.protocol === "http:" && ["localhost", "127.0.0.1", "[::1]"].includes(base.hostname)))) throw new VoiceRequestFailure("configuration");
    // 認証基盤のエラーに埋もれないよう、先に Vo の応答を確認する。
    let health: Response;
    try { health = await fetchImpl(new URL("/health", base), { redirect: "manual", signal: AbortSignal.timeout(5000) }); }
    catch { throw new VoiceRequestFailure("down"); }
    if (!health.ok) throw new VoiceRequestFailure(health.status >= 500 ? "down" : "protocol");
    const issued = await token();
    if (!issued.ok) throw new VoiceRequestFailure("auth");
    const submitting = theme !== undefined;
    let response: Response;
    try {
      response = await fetchImpl(new URL(`/api/personas/impression-requests${submitting ? "" : `/${encodeURIComponent(requestId)}`}`, base), {
        method: submitting ? "POST" : "GET", redirect: "manual", signal: AbortSignal.timeout(10000),
        headers: { authorization: `Bearer ${issued.token}`, "content-type": "application/json" },
        ...(submitting ? { body: JSON.stringify({ requestId, theme }) } : {}),
      });
    } catch { throw new VoiceRequestFailure("down", submitting); }
    if (!submitting && response.status === 404) return { accepted: false, requestId };
    if (!response.ok) throw new VoiceRequestFailure(response.status === 401 || response.status === 403 ? "auth" : response.status >= 500 ? "down" : "protocol", submitting && response.status >= 500);
    let body: { data?: { request?: { requestId?: string; status?: string } } };
    try { body = await response.json() as typeof body; } catch { throw new VoiceRequestFailure("protocol", submitting); }
    const receipt = body.data?.request;
    if (receipt?.requestId !== requestId || !["accepted", "completed", "cancelled"].includes(receipt.status ?? "")) throw new VoiceRequestFailure("protocol", submitting);
    return { accepted: receipt.status === "accepted" || receipt.status === "completed", requestId };
  }
  return { submit: ({ requestId, theme }) => request(requestId, theme), status: requestId => request(requestId) };
}

export const configuredVoiceRequest = createVoiceRequestClient(
  () => getConfig().flow.userVoices.voluptasBaseUrl,
  createServiceTokenProvider({ targetProjectKey: "voluptas", config: serviceTokenClientConfigFromEnv,
    fetchImpl: (input, init) => fetch(input, { ...init, signal: AbortSignal.timeout(10000) }),
  }),
);
