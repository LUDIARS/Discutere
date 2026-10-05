import type { KeyObject } from "node:crypto";

import { ed25519PublicKeyFromRaw } from "./paseto-v4-public.js";

/**
 * Cernere の `/.well-known/cernere-public-key` を取得し、 process memory に
 * キャッシュする。 応答は現行鍵 + ローテーション中の旧鍵の一覧。
 * Cernere は `cache-control: max-age=600` を返すので、 同じ 10 分で取り直す。
 */
const CACHE_TTL_MS = 10 * 60 * 1000;
// 取得失敗の直後に毎リクエストで Cernere を叩かないための短い待ち。
const FAILURE_BACKOFF_MS = 30 * 1000;

export type PublicKeyLoader = () => Promise<KeyObject[]>;

interface WellKnownKey {
  public_key?: unknown;
}

export function parseWellKnownKeys(body: unknown): KeyObject[] {
  const keys = (body as { keys?: unknown })?.keys;
  if (!Array.isArray(keys)) throw new Error("cernere public key response has no keys");
  return keys
    .map((entry) => (entry as WellKnownKey)?.public_key)
    .filter((value): value is string => typeof value === "string" && value.length > 0)
    .map((value) => ed25519PublicKeyFromRaw(Buffer.from(value, "base64")));
}

export function createCernerePublicKeyLoader({
  baseUrl,
  fetchImpl = fetch,
  now = () => Date.now(),
}: {
  baseUrl: () => string;
  fetchImpl?: typeof fetch;
  now?: () => number;
}): PublicKeyLoader {
  let cached: { keys: KeyObject[]; expiresAt: number } | null = null;
  let failedUntil = 0;
  return async () => {
    const at = now();
    if (cached && at < cached.expiresAt) return cached.keys;
    if (at < failedUntil) throw new Error("cernere public key fetch is backing off");
    const base = baseUrl().trim();
    if (!base) throw new Error("CERNERE_BASE_URL is not set");
    try {
      const response = await fetchImpl(new URL("/.well-known/cernere-public-key", base), {
        redirect: "manual",
        headers: { accept: "application/json" },
      });
      if (!response.ok) throw new Error(`cernere public key fetch failed with status ${response.status}`);
      const keys = parseWellKnownKeys(await response.json());
      if (keys.length === 0) throw new Error("cernere public key response is empty");
      cached = { keys, expiresAt: at + CACHE_TTL_MS };
      return keys;
    } catch (error) {
      failedUntil = at + FAILURE_BACKOFF_MS;
      throw error;
    }
  };
}
