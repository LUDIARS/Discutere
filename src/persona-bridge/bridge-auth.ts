/**
 * Voluptas → Discutere ブリッジの呼び出し認証 (認証集約 P4)。
 *
 * 同じ `Authorization: Bearer` で次のどちらかを受ける:
 *   - Cernere service token (PASETO v4.public。aud = Discutere の storage_slug、scope = persona-bridge:write)
 *     → Cernere 公開鍵で検証する。Discutere 自身は Cernere の credential を持たなくてよい (検証だけ)。
 *   - 従来の固定トークン (DISCUTERE_PERSONA_BRIDGE_TOKEN、32 文字以上。P5 で撤去予定)
 * 固定トークンが短い/未設定なら 503 (default-deny)。理由の詳細は応答に出さない。
 */

import { timingSafeEqual } from "node:crypto";

import type { PublicKeyLoader } from "../cernere-service-token/cernere-public-keys.js";
import { isPasetoV4Public } from "../cernere-service-token/paseto-v4-public.js";
import { verifyServiceToken } from "../cernere-service-token/service-token-verify.js";

/** 推測可能な短い token を「設定済み」と見なさない。 */
export const MIN_BRIDGE_TOKEN_LENGTH = 32;
/** Cernere service token でブリッジを呼ぶのに要る scope。 */
export const PERSONA_BRIDGE_SERVICE_SCOPE = "persona-bridge:write";

export interface BridgeAuthOptions {
  /** 固定トークン (DISCUTERE_PERSONA_BRIDGE_TOKEN)。 */
  token: () => string;
  /** service token の aud (= CERNERE_PROJECT_STORAGE_SLUG)。 */
  serviceAudience: () => string;
  loadServiceTokenKeys: PublicKeyLoader;
  now: () => number;
  /** ログの接頭辞 (例 "persona-bridge")。 */
  label: string;
}

export type BridgeAuthResult = { ok: true } | { ok: false; status: 401 | 403 | 503; error: string };

function secureMatch(actual: string, expected: string): boolean {
  const left = Buffer.from(actual);
  const right = Buffer.from(expected);
  return left.length === right.length && timingSafeEqual(left, right);
}

export function bearerToken(header: string | undefined): string {
  const match = /^Bearer ([^\s]+)$/.exec(header ?? "");
  return match?.[1] ?? "";
}

/** Authorization ヘッダを検証する。 */
export async function authorizeBridgeRequest(
  header: string | undefined,
  opts: BridgeAuthOptions
): Promise<BridgeAuthResult> {
  const presented = bearerToken(header);
  if (isPasetoV4Public(presented)) {
    const verdict = await verifyServiceToken({
      token: presented,
      audience: opts.serviceAudience(),
      requiredScope: PERSONA_BRIDGE_SERVICE_SCOPE,
      loadKeys: opts.loadServiceTokenKeys,
      nowMs: opts.now(),
    });
    if (verdict.ok) return { ok: true };
    if (verdict.status === 503) {
      console.warn(`[${opts.label}] service token verification unavailable (reason=${verdict.reason})`);
      return { ok: false, status: 503, error: "persona bridge service token verification is not configured" };
    }
    if (verdict.status === 403) return { ok: false, status: 403, error: "forbidden" };
    return { ok: false, status: 401, error: "unauthorized" };
  }
  const expected = opts.token();
  if (expected.length < MIN_BRIDGE_TOKEN_LENGTH) {
    return {
      ok: false,
      status: 503,
      error: `persona bridge is not configured (DISCUTERE_PERSONA_BRIDGE_TOKEN must be at least ${MIN_BRIDGE_TOKEN_LENGTH} characters)`,
    };
  }
  if (!secureMatch(presented, expected)) return { ok: false, status: 401, error: "unauthorized" };
  return { ok: true };
}
