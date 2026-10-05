import type { PublicKeyLoader } from "./cernere-public-keys.js";
import { verifyPasetoV4Public } from "./paseto-v4-public.js";

/**
 * Cernere service token (PASETO v4.public, kind=service) の受け側検証。
 *
 * 照合するのは署名・`kind`・`exp`・`aud` (= 自分の storage_slug)・要求 scope だけで、
 * 呼出元名 (`sub`) では分岐しない (Cernere spec/feature/service-token.md)。
 * 参照実装: Cernere `verifyServiceTokenPaseto` / `hasServiceScope`。
 */
export interface ServiceTokenClaims {
  kind: "service";
  sub: string;
  aud: string;
  scope: string[];
  exp: string;
}

export type ServiceTokenVerdict =
  | { ok: true; claims: ServiceTokenClaims }
  /** 401: 署名不正・期限切れ・aud 不一致・kind 不一致・形式不正。 */
  | { ok: false; status: 401; reason: string }
  /** 403: 正しい service token だが endpoint の scope が無い。 */
  | { ok: false; status: 403; reason: string }
  /** 503: 受け側の設定不足 / 公開鍵が取れない (token の正否を判定できない)。 */
  | { ok: false; status: 503; reason: string };

function readClaims(payload: Record<string, unknown>): ServiceTokenClaims | null {
  const { kind, sub, aud, scope, exp } = payload;
  if (kind !== "service") return null;
  if (typeof sub !== "string" || !sub) return null;
  if (typeof aud !== "string" || typeof exp !== "string") return null;
  if (!Array.isArray(scope) || !scope.every((value) => typeof value === "string")) return null;
  return { kind, sub, aud, scope: scope as string[], exp };
}

export async function verifyServiceToken({
  token,
  audience,
  requiredScope,
  loadKeys,
  nowMs,
}: {
  token: string;
  audience: string;
  requiredScope: string;
  loadKeys: PublicKeyLoader;
  nowMs: number;
}): Promise<ServiceTokenVerdict> {
  if (!audience.trim()) {
    return { ok: false, status: 503, reason: "audience_not_configured" };
  }
  let keys;
  try {
    keys = await loadKeys();
  } catch {
    return { ok: false, status: 503, reason: "public_key_unavailable" };
  }
  let payload: Record<string, unknown>;
  try {
    payload = verifyPasetoV4Public(token, keys);
  } catch {
    return { ok: false, status: 401, reason: "invalid_signature" };
  }
  const claims = readClaims(payload);
  if (!claims) return { ok: false, status: 401, reason: "not_service_token" };
  const expiresAt = Date.parse(claims.exp);
  if (!Number.isFinite(expiresAt) || expiresAt <= nowMs) {
    return { ok: false, status: 401, reason: "expired" };
  }
  if (claims.aud !== audience) return { ok: false, status: 401, reason: "audience_mismatch" };
  if (!claims.scope.includes(requiredScope)) {
    return { ok: false, status: 403, reason: "scope_missing" };
  }
  return { ok: true, claims };
}
