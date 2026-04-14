/**
 * Discutere AuthContext
 *
 * Cernere Composite ログインで取得した service_token (HttpOnly Cookie) を
 * 使って Discutere backend の /api/auth/me を呼び、ユーザー情報を保持する。
 */

import {
  createContext,
  useContext,
  useState,
  useEffect,
  useCallback,
  type ReactNode,
} from "react";
import { API_BASE } from "../lib/constants";

export interface AuthUser {
  id: string;
  name: string;
  email: string | null;
  role: string;
}

interface AuthContextValue {
  user: AuthUser | null;
  loading: boolean;
  loginWithPopup: () => Promise<void>;
  /** Redirect / 半SPA からの authCode を service_token に交換する */
  completeLogin: (authCode: string) => Promise<void>;
  logout: () => Promise<void>;
}

const AuthContext = createContext<AuthContextValue | null>(null);

const STORAGE_KEY = "discutere.user";

function loadStoredUser(): AuthUser | null {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    return raw ? (JSON.parse(raw) as AuthUser) : null;
  } catch {
    return null;
  }
}

function saveStoredUser(u: AuthUser | null): void {
  if (u) localStorage.setItem(STORAGE_KEY, JSON.stringify(u));
  else localStorage.removeItem(STORAGE_KEY);
}

async function fetchMe(): Promise<AuthUser | null> {
  const res = await fetch(`${API_BASE}/api/auth/me`, { credentials: "include" });
  if (!res.ok) return null;
  return (await res.json()) as AuthUser;
}

async function exchangeAuthCode(authCode: string): Promise<AuthUser> {
  const res = await fetch(`${API_BASE}/api/auth/exchange`, {
    method: "POST",
    credentials: "include",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ code: authCode }),
  });
  const data = await res.json();
  if (!res.ok) throw new Error((data && data.error) || "Auth code exchange failed");
  const u = data.user as { id: string; displayName: string; email: string | null; role: string };
  return { id: u.id, name: u.displayName, email: u.email, role: u.role };
}

export function AuthProvider({ children }: { children: ReactNode }) {
  const [user, setUser] = useState<AuthUser | null>(loadStoredUser);
  const [loading, setLoading] = useState<boolean>(() => !!loadStoredUser());

  // 起動時: Cookie が生きていれば /me で確認、無効なら localStorage を消す
  useEffect(() => {
    if (!user) {
      setLoading(false);
      return;
    }
    fetchMe()
      .then((me) => {
        if (me) {
          setUser(me);
          saveStoredUser(me);
        } else {
          setUser(null);
          saveStoredUser(null);
        }
      })
      .finally(() => setLoading(false));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const completeLogin = useCallback(async (authCode: string) => {
    const u = await exchangeAuthCode(authCode);
    setUser(u);
    saveStoredUser(u);
  }, []);

  /**
   * Popup ログイン
   * 1. Backend の /api/auth/login-url から Cernere ログイン URL を取得
   * 2. Popup を開き、Cernere ログイン → /composite/callback に postMessage で authCode 返却
   * 3. authCode を Backend で交換 → service_token Cookie が設定される
   */
  const loginWithPopup = useCallback(async () => {
    const origin = window.location.origin;
    const res = await fetch(
      `${API_BASE}/api/auth/login-url?origin=${encodeURIComponent(origin)}`,
    );
    const data = await res.json();
    if (!res.ok) throw new Error(data?.error || "Login URL fetch failed");
    const url = data.url as string;

    const popup = window.open(url, "discutere-login", "width=500,height=700");
    if (!popup) throw new Error("Popup blocked. Please allow popups.");

    const authCode = await new Promise<string>((resolve, reject) => {
      const timer = setInterval(() => {
        if (popup.closed) {
          clearInterval(timer);
          window.removeEventListener("message", handler);
          reject(new Error("Login popup was closed."));
        }
      }, 500);

      const handler = (event: MessageEvent) => {
        if (typeof event.data !== "object" || !event.data) return;
        const { type, code } = event.data as { type?: string; code?: string };
        if (type !== "cernere-auth-code" || !code) return;
        clearInterval(timer);
        window.removeEventListener("message", handler);
        try { popup.close(); } catch { /* ignore */ }
        resolve(code);
      };
      window.addEventListener("message", handler);
    });

    await completeLogin(authCode);
  }, [completeLogin]);

  const logout = useCallback(async () => {
    await fetch(`${API_BASE}/api/auth/logout`, {
      method: "POST",
      credentials: "include",
    });
    setUser(null);
    saveStoredUser(null);
  }, []);

  return (
    <AuthContext.Provider value={{ user, loading, loginWithPopup, completeLogin, logout }}>
      {children}
    </AuthContext.Provider>
  );
}

export function useAuth(): AuthContextValue {
  const ctx = useContext(AuthContext);
  if (!ctx) throw new Error("useAuth must be used within AuthProvider");
  return ctx;
}
