/**
 * Claude Code のサブスク OAuth トークンを読む (E: SDK をサブスクで使う)。
 *
 * Claude Code は `~/.claude/.credentials.json` に `claudeAiOauth`
 * { accessToken, refreshToken, expiresAt, scopes, subscriptionType } を保存し、
 * 背景で鮮度を維持する。本モジュールはそれを読むだけ (リフレッシュは Claude Code 任せ)。
 * 期限切れ / 不在なら null を返し、呼び出し側は claude-p 等にフォールバックする。
 *
 * OAuth トークンは `Authorization: Bearer` + `anthropic-beta: oauth-2025-04-20` で
 * `/v1/messages` に通る (claude-api skill)。x-api-key (従量) とは別経路。
 */

import { readFileSync } from "node:fs";
import { homedir } from "node:os";
import path from "node:path";

/** OAuth トークンに付ける beta ヘッダ (claude-api skill)。 */
export const OAUTH_BETA_HEADER = "oauth-2025-04-20";

interface ClaudeCodeCreds {
  claudeAiOauth?: {
    accessToken?: string;
    expiresAt?: number;
  };
}

const DEFAULT_PATH = path.join(homedir(), ".claude", ".credentials.json");
/** 期限のマージン (この秒数前に切れ扱いにして取り直す)。 */
const EXPIRY_MARGIN_MS = 60_000;
/** ファイル再読込の最短間隔 (TTL キャッシュ)。 */
const REREAD_MS = 30_000;

let cached: { token: string; expiresAt: number } | null = null;
let lastReadAt = 0;

/** epochSec/epochMs どちらでも ms に正規化する。 */
function toMs(v: number): number {
  return v < 1e12 ? v * 1000 : v;
}

/**
 * 有効な Claude Code OAuth access token を返す。期限切れ / 不在なら null。
 * @param credsPath 既定 `~/.claude/.credentials.json` (テスト用に差し替え可)。
 */
export function readClaudeCodeToken(credsPath: string = DEFAULT_PATH): string | null {
  const now = Date.now();
  // TTL 内かつキャッシュが有効ならファイルを読まない。
  if (cached && now - lastReadAt < REREAD_MS && cached.expiresAt - EXPIRY_MARGIN_MS > now) {
    return cached.token;
  }
  try {
    const raw = readFileSync(credsPath, "utf8");
    const creds = JSON.parse(raw) as ClaudeCodeCreds;
    const oauth = creds.claudeAiOauth;
    lastReadAt = now;
    if (!oauth?.accessToken || typeof oauth.expiresAt !== "number") {
      cached = null;
      return null;
    }
    const expiresAt = toMs(oauth.expiresAt);
    if (expiresAt - EXPIRY_MARGIN_MS <= now) {
      cached = null;
      return null;
    }
    cached = { token: oauth.accessToken, expiresAt };
    return cached.token;
  } catch {
    cached = null;
    return null;
  }
}

/** テスト用: キャッシュをリセットする。 */
export function _resetClaudeCodeTokenCache(): void {
  cached = null;
  lastReadAt = 0;
}
