import { useEffect, useState } from "react";
import { useNavigate, useSearchParams } from "react-router-dom";
import { useAuth } from "../contexts/AuthContext";

/**
 * Cernere からのリダイレクト先 (/composite/callback)
 *
 * URL クエリ ?code=... を取り出し、postMessage で親ウィンドウ (popup opener) に
 * 通知する。ポップアップで開かれていない場合 (リダイレクトモード) は
 * completeLogin → "/" にリダイレクト。
 */
export function CallbackPage() {
  const [params] = useSearchParams();
  const navigate = useNavigate();
  const { completeLogin } = useAuth();
  const [error, setError] = useState("");

  useEffect(() => {
    const code = params.get("code");
    if (!code) {
      setError("authorization code not found");
      return;
    }

    // Popup モード: 親ウィンドウに code を渡して閉じる
    if (window.opener && window.opener !== window) {
      window.opener.postMessage({ type: "cernere-auth-code", code }, window.location.origin);
      window.close();
      return;
    }

    // Redirect モード: 自身で交換して "/" へ
    completeLogin(code)
      .then(() => navigate("/", { replace: true }))
      .catch((err: unknown) => {
        setError(err instanceof Error ? err.message : "Authentication failed");
      });
  }, [params, completeLogin, navigate]);

  return (
    <div style={{ padding: "2rem", textAlign: "center", color: "var(--text, #c9d1d9)" }}>
      {error ? <p style={{ color: "#f85149" }}>{error}</p> : <p>認証処理中…</p>}
    </div>
  );
}
