/**
 * Cernere Composite — ユーザー認証フロー (Discutere)
 *
 * Cernere のポップアップ/リダイレクトログインで得た auth_code を
 * Cernere の /api/auth/exchange で accessToken / user 情報に交換し、
 * Discutere 自身の service_token を発行する。
 */

import jwt from "jsonwebtoken";

interface CernereUser {
  id: string;
  displayName: string;
  email: string | null;
  role: string;
}

export interface ExchangeResult {
  serviceToken: string;
  user: CernereUser;
}

const TOKEN_EXPIRES_IN_SECONDS = 900; // 15分

function getCernereUrl(): string {
  return process.env.CERNERE_URL ?? "";
}

function getJwtSecret(): string {
  return process.env.JWT_SECRET ?? "";
}

/** Cernere Composite ログイン URL を返す */
export function getLoginUrl(origin: string): string | null {
  const cernereUrl = getCernereUrl();
  if (!cernereUrl) return null;
  return `${cernereUrl}/composite/login?origin=${encodeURIComponent(origin)}`;
}

/** Composite が有効か */
export function isCompositeEnabled(): boolean {
  return !!getCernereUrl() && !!getJwtSecret();
}

/** auth_code を Cernere で交換し、service_token を発行する */
export async function exchangeAuthCode(authCode: string): Promise<ExchangeResult> {
  const cernereUrl = getCernereUrl();
  if (!cernereUrl) throw new Error("Cernere Composite is not configured");

  const res = await fetch(`${cernereUrl}/api/auth/exchange`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ code: authCode }),
  });

  if (!res.ok) {
    const body = await res.text().catch(() => "");
    throw new Error(`Cernere exchange failed: ${res.status} ${body}`);
  }

  const data = await res.json() as {
    accessToken: string;
    refreshToken: string;
    user: CernereUser;
  };

  const serviceToken = issueServiceToken(data.user);
  return { serviceToken, user: data.user };
}

function issueServiceToken(user: CernereUser): string {
  const secret = getJwtSecret();
  if (!secret) throw new Error("JWT_SECRET is not configured");

  return jwt.sign(
    {
      sub: user.id,
      name: user.displayName,
      email: user.email,
      role: user.role,
    },
    secret,
    {
      expiresIn: TOKEN_EXPIRES_IN_SECONDS,
      issuer: "discutere",
    }
  );
}
